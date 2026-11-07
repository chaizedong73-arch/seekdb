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

#include "sql/engine/expr/ob_expr_file_table_func.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/resolver/expr/ob_raw_expr.h"
#include "sql/session/ob_sql_session_info.h"
#include "lib/string/ob_sql_string.h"
#include <limits.h>
#include <stdlib.h>

namespace oceanbase
{
using namespace common;
namespace sql
{

FileTableFuncExpr::FileTableFuncExpr(ObIAllocator &alloc, ObExprOperatorType type, const char *name)
    : ObFuncExprOperator(alloc, type, name, MORE_THAN_ZERO, NOT_VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
{}

FileTableFuncExpr::~FileTableFuncExpr()
{}

int FileTableFuncExpr::calc_result_typeN(ObExprResType &type, ObExprResType *args,
                                        int64_t count, ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  if (count < 1 || count % 2 != 1) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    args[0].set_calc_type(ObVarcharType);
  }
  type.set_varchar();
  type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  return ret;
}

int FileTableFuncExpr::cg_expr(ObExprCGCtx &expr_cg_ctx,
                              const ObRawExpr &raw_expr,
                              ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  UNUSED(expr_cg_ctx);
  if (OB_UNLIKELY(raw_expr.get_param_count() < 1 || raw_expr.get_param_count() % 2 != 1)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(raw_expr.get_param_expr(0))) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!raw_expr.get_param_expr(0)->is_const_expr()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT,
        "file table function. The argument should be a constant string path");
  }
  rt_expr.eval_func_ = FileTableFuncExpr::eval_not_supported;
  return ret;
}

int FileTableFuncExpr::get_read_options(const ObRawExpr &expr, FileReadOptions &options)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObObj, 16> values;
  for (int64_t i = 0; OB_SUCC(ret) && i < expr.get_param_count(); ++i) {
    const ObRawExpr *arg = expr.get_param_expr(i);
    ObObj value;
    if (OB_ISNULL(arg)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (arg->is_const_raw_expr()) {
      value = static_cast<const ObConstRawExpr *>(arg)->get_value();
    } else if (arg->get_expr_type() == T_OP_NEG && arg->get_param_count() == 1
               && OB_NOT_NULL(arg->get_param_expr(0))
               && arg->get_param_expr(0)->is_const_raw_expr()) {
      const ObObj &operand = static_cast<const ObConstRawExpr *>(arg->get_param_expr(0))->get_value();
      if (!operand.is_int() || operand.get_int() < 0) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        value.set_int(-operand.get_int());
      }
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    if (OB_SUCC(ret) && OB_FAIL(values.push_back(value))) {
    }
  }
  if (OB_SUCC(ret)) { ret = parse_file_read_options(values, options); }
  return ret;
}

int FileTableFuncExpr::get_read_options(const ObExpr &expr, ObEvalCtx &ctx, FileReadOptions &options)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObObj, 16> values;
  for (int64_t i = 0; OB_SUCC(ret) && i < expr.arg_cnt_; ++i) {
    ObDatum *datum = nullptr;
    ObObj value;
    if (OB_FAIL(expr.args_[i]->eval(ctx, datum))) {
    } else if (OB_ISNULL(datum)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(datum->to_obj(value, expr.args_[i]->obj_meta_))) {
    } else if (OB_FAIL(values.push_back(value))) {
    }
  }
  if (OB_SUCC(ret)) { ret = parse_file_read_options(values, options); }
  return ret;
}

int FileTableFuncExpr::validate_path(ObSQLSessionInfo &session, ObIAllocator &allocator,
                                         FileTableFormat format, ObString &path,
                                         FileReadOptions &options, bool allow_missing_file)
{
  int ret = OB_SUCCESS;
  int64_t enabled = 0;
  ObString root;
  char root_input[PATH_MAX] = {};
  char path_input[PATH_MAX] = {};
  char real_root[PATH_MAX] = {};
  char real_path[PATH_MAX] = {};
  ObString suffix;
  if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (enabled <= 0) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_SECURE_FILE_PRIV, root))) {
  } else if (root.empty() || root.case_compare("NULL") == 0) {
    ret = OB_ERR_NO_PRIVILEGE;
  } else if (path.empty() || root.length() >= PATH_MAX || path.length() >= PATH_MAX
             || nullptr != MEMCHR(path.ptr(), '\0', path.length())
             || nullptr != MEMCHR(root.ptr(), '\0', root.length())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(root_input, root.ptr(), root.length());
    MEMCPY(path_input, path.ptr(), path.length());
    if (format == FILE_TABLE_GLOB || allow_missing_file) {
      char *sep = strrchr(path_input, '/');
      if (nullptr == sep) {
        suffix = path;
        path_input[0] = '.';
        path_input[1] = '\0';
      } else {
        suffix.assign_ptr(path.ptr() + (sep - path_input) + 1,
                          path.length() - (sep - path_input) - 1);
        if (sep == path_input) { sep[1] = '\0'; } else { *sep = '\0'; }
      }
    }
    if (nullptr == realpath(root_input, real_root) || nullptr == realpath(path_input, real_path)) {
      ret = OB_ERR_NO_PRIVILEGE;
    } else {
      ObString canonical_root(real_root);
      ObString canonical_path(real_path);
      if (!canonical_path.prefix_match(canonical_root)
          || (canonical_root != "/" && canonical_path.length() > canonical_root.length()
              && canonical_path[canonical_root.length()] != '/')) {
        ret = OB_ERR_NO_PRIVILEGE;
      } else if (OB_FAIL(ob_write_string(allocator, canonical_root, options.secure_root_))) {
      } else {
        ObSqlString resolved;
        if (OB_FAIL(resolved.append(canonical_path))) {
        } else if ((format == FILE_TABLE_GLOB || allow_missing_file)
                   && (OB_FAIL(resolved.append("/")) || OB_FAIL(resolved.append(suffix)))) {
        } else if (OB_FAIL(ob_write_string(allocator, resolved.string(), path))) {
        }
      }
    }
  }
  return ret;
}

int FileTableFuncExpr::eval_not_supported(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res_datum)
{
  UNUSED(expr);
  UNUSED(ctx);
  UNUSED(res_datum);
  int ret = OB_NOT_SUPPORTED;
  LOG_USER_ERROR(OB_NOT_SUPPORTED, "eval file table function");
  LOG_WARN("file table function can only be used in FROM clause", K(ret));
  return ret;
}

bool FileTableFuncExpr::is_file_table_func_type(ObExprOperatorType type)
{
  return type >= T_FUN_SYS_READ_CSV && type <= T_FUN_SYS_GLOB;
}

FileTableFormat FileTableFuncExpr::get_format_by_expr_type(ObExprOperatorType type)
{
  FileTableFormat format = FILE_TABLE_MAX_FORMAT;
  switch (type) {
    case T_FUN_SYS_READ_CSV:
      format = FILE_TABLE_CSV;
      break;
    case T_FUN_SYS_READ_JSON:
      format = FILE_TABLE_JSON;
      break;
    case T_FUN_SYS_READ_NDJSON:
      format = FILE_TABLE_NDJSON;
      break;
    case T_FUN_SYS_READ_PARQUET:
      format = FILE_TABLE_PARQUET;
      break;
    case T_FUN_SYS_GLOB:
      format = FILE_TABLE_GLOB;
      break;
    default:
      break;
  }
  return format;
}

} /* namespace sql */
} /* namespace oceanbase */
