/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or content is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content = arg_content->get_string();
    if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      if (arg_dim->is_null()) {
        // null dimension falls back to the default dimension config
      } else {
        dim = arg_dim->get_int();
        if (dim <= 0) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
          res.set_null();
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

// the max input count of one embedding request, inputs of a batch are
// split into several requests if there are too many rows
static const int64_t AI_EMBED_MAX_REQUEST_SIZE = 64;

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t batch_size)
{
  INIT_SUCC(ret);
  // evaluate all the args of the batch first
  for (int64_t i = 0; OB_SUCC(ret) && i < expr.arg_cnt_; i++) {
    if (OB_FAIL(expr.args_[i]->eval_batch(ctx, skip, batch_size))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (expr.arg_cnt_ != 2 && expr.arg_cnt_ != 3) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObDatumVector model_datums = expr.args_[0]->locate_expr_datumvector(ctx);
    ObDatumVector content_datums = expr.args_[1]->locate_expr_datumvector(ctx);
    ObDatumVector dim_datums;
    if (expr.arg_cnt_ == 3) {
      dim_datums = expr.args_[2]->locate_expr_datumvector(ctx);
    }
    ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
    ObDatumVector res_datums = expr.locate_expr_datumvector(ctx);
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));

    // collect the rows that can be evaluated in one batched request.
    // rows with null model id or null content are propagated as null
    // results and are not put into the request.
    ObArray<int64_t> row_ids;
    ObArray<ObString> contents;
    bool can_batch = true;
    ObString first_model_id;
    int64_t first_dim = 0;
    for (int64_t j = 0; OB_SUCC(ret) && j < batch_size; j++) {
      if (skip.at(j) || eval_flags.at(j)) {
        continue;
      }
      ObDatum *model_datum = model_datums.at(j);
      ObDatum *content_datum = content_datums.at(j);
      if (model_datum->is_null() || content_datum->is_null()) {
        res_datums.at(j)->set_null();
        eval_flags.set(j);
      } else {
        ObString model_id = model_datum->get_string();
        ObString content = content_datum->get_string();
        int64_t cur_dim = 0;
        if (model_id.empty() || content.empty()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
        } else if (expr.arg_cnt_ == 3 && !dim_datums.at(j)->is_null()) {
          cur_dim = dim_datums.at(j)->get_int();
          if (cur_dim <= 0) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
          }
        }
        if (OB_SUCC(ret)) {
          if (first_model_id.empty()) {
            first_model_id = model_id;
            first_dim = cur_dim;
            if (OB_FAIL(row_ids.push_back(j)) || OB_FAIL(contents.push_back(content))) {
            }
          } else if (model_id.compare(first_model_id) != 0 || cur_dim != first_dim) {
            // rows with different model ids or dimensions cannot be
            // evaluated in one batched request
            can_batch = false;
            break;
          } else if (OB_FAIL(row_ids.push_back(j)) || OB_FAIL(contents.push_back(content))) {
          }
        }
      }
    }

    if (OB_FAIL(ret)) {
    } else if (!can_batch) {
      // fall back to the row-by-row evaluation
      for (int64_t j = 0; OB_SUCC(ret) && j < batch_size; j++) {
        if (skip.at(j) || eval_flags.at(j)) {
          continue;
        }
        ObEvalCtx::BatchInfoScopeGuard batch_info_guard(ctx);
        batch_info_guard.set_batch_idx(j);
        if (OB_FAIL(expr.eval_func_(expr, ctx, *(res_datums.at(j))))) {
        } else {
          eval_flags.set(j);
        }
      }
    } else if (contents.count() > 0) {
      // resolve the model info and endpoint once for the whole batch
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo resolved_endpoint;
      const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
      query::ObIAiEndpointResolver *endpoint_resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, first_model_id, info))) {
      } else if (OB_ISNULL(endpoint_resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                     first_model_id, temp_allocator, resolved_endpoint))) {
      } else {
        ObJsonObject *config = nullptr;
        if (first_dim > 0) {
          ObJsonInt *dim_json = nullptr;
          if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
          } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, first_dim, dim_json))) {
          } else if (OB_FAIL(config->add("dimensions", dim_json))) {
          }
        }
        if (OB_SUCC(ret)) {
          ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
          // split the rows into several requests if there are too many
          for (int64_t start = 0; OB_SUCC(ret) && start < contents.count(); start += AI_EMBED_MAX_REQUEST_SIZE) {
            int64_t end = start + AI_EMBED_MAX_REQUEST_SIZE;
            if (end > contents.count()) {
              end = contents.count();
            }
            ObArray<ObString> request_contents;
            ObArray<ObString> request_results;
            for (int64_t k = start; OB_SUCC(ret) && k < end; k++) {
              if (OB_FAIL(request_contents.push_back(contents.at(k)))) {
              }
            }
            if (OB_FAIL(ret)) {
            } else if (OB_FAIL(model.call_dense_embedding_vector_v2(request_contents, config, request_results))) {
            } else if (request_results.count() != request_contents.count()) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              for (int64_t k = 0; OB_SUCC(ret) && k < request_results.count(); k++) {
                int64_t row_idx = row_ids.at(start + k);
                ObString result = request_results.at(k);
                char *res_buf = expr.get_str_res_mem(ctx, result.length(), row_idx);
                if (OB_ISNULL(res_buf)) {
                  ret = OB_ALLOCATE_MEMORY_FAILED;
                } else {
                  MEMCPY(res_buf, result.ptr(), result.length());
                  res_datums.at(row_idx)->set_string(res_buf, result.length());
                  eval_flags.set(row_idx);
                }
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx,
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    // batched evaluation: rows within one batch are sent in batched requests
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
