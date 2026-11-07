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

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/expr/ob_expr_file_table_func.h"


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("value expr is not init", K(ret));
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else if (FileTableFuncExpr::is_file_table_func_type(MY_SPEC.value_expr_->type_)) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  if (OB_SUCC(ret)
      && FileTableFuncExpr::is_file_table_func_type(MY_SPEC.value_expr_->type_)) {
    if (OB_FAIL(init_file_table_reader())) {
      LOG_WARN("failed to init file table reader", K(ret));
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
    // A rescan belongs to the same statement and must see the same file.
    if (OB_SUCC(ret) && OB_NOT_NULL(file_reader_)
        && FileTableFuncExpr::is_file_table_func_type(MY_SPEC.value_expr_->type_)) {
      FileTableFileStat cur_stat;
      bool file_changed = false;
      if (OB_FAIL(file_reader_->get_file_stat(cur_stat))) {
        LOG_WARN("failed to get file stat on rescan", K(ret));
      } else if (cur_stat != file_stat_) {
        file_changed = true;
      }
      if (OB_SUCC(ret)) {
        if (file_changed) {
          ret = OB_INVALID_DATA;
          LOG_WARN("file changed during statement execution", K(ret));
        } else if (OB_FAIL(file_reader_->reuse())) {
          LOG_WARN("failed to reuse file table reader on rescan", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  if (OB_NOT_NULL(file_reader_)) {
    file_reader_->close();
  }
  return ret;
}

void ObFunctionTableOp::destroy()
{
  if (OB_NOT_NULL(file_reader_)) {
    file_reader_->close();
    file_reader_->~IFileTableReader();
    file_reader_ = NULL;
  }
  ObOperator::destroy();
}

int ObFunctionTableOp::init_file_table_reader()
{
  int ret = OB_SUCCESS;
  ObExpr *value_expr = MY_SPEC.value_expr_;
  CK (OB_NOT_NULL(value_expr));
  if (OB_SUCC(ret)) {
    FileTableFormat format = FileTableFuncExpr::get_format_by_expr_type(value_expr->type_);
    if (OB_UNLIKELY(format >= FILE_TABLE_MAX_FORMAT)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid file table function type", K(value_expr->type_), K(ret));
    } else if (OB_UNLIKELY(value_expr->arg_cnt_ < 1) || OB_ISNULL(value_expr->args_[0])) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("file table function should have a path argument", K(value_expr->arg_cnt_), K(ret));
    } else {
      // the path argument is a constant string
      ObDatum *path_datum = NULL;
      if (OB_FAIL(value_expr->args_[0]->eval(eval_ctx_, path_datum))) {
        LOG_WARN("failed to eval path expr of file table function", K(ret));
      } else if (OB_ISNULL(path_datum) || path_datum->is_null()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "path of file table function");
      } else {
        ObString path = path_datum->get_string();
        FileReadOptions options;
        if (OB_NOT_NULL(file_reader_)) {
          file_reader_->close();
        }
        if (OB_FAIL(FileTableFuncExpr::get_read_options(*value_expr, eval_ctx_, options))) {
          LOG_WARN("invalid file reader options", K(ret));
        } else if (OB_ISNULL(ctx_.get_my_session())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(FileTableFuncExpr::validate_path(*ctx_.get_my_session(),
                            ctx_.get_allocator(), format, path, options))) {
          LOG_WARN("file access denied", K(ret));
        } else if (OB_FAIL(create_file_table_reader(format, ctx_.get_allocator(), file_reader_))) {
          LOG_WARN("failed to create file table reader", K(format), K(ret));
        } else if (OB_FAIL(file_reader_->init(path, options))) {
          LOG_WARN("failed to init file table reader", K(path), K(ret));
        } else if (OB_FAIL(file_reader_->get_file_stat(file_stat_))) {
          LOG_WARN("failed to get file stat", K(path), K(ret));
        } else if (OB_UNLIKELY(MY_SPEC.column_exprs_.count() < 1)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("file table has no column", K(MY_SPEC.column_exprs_.count()), K(ret));
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_file()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_ISNULL(file_reader_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file table reader is not inited", K(ret));
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    bool has_next = false;
    FileTableFileStat current_stat;
    file_row_.reuse();
    if (OB_FAIL(file_reader_->get_file_stat(current_stat))) {
    } else if (current_stat != file_stat_) {
      ret = OB_INVALID_DATA;
      LOG_WARN("file changed during statement execution", K(ret));
    } else if (OB_FAIL(file_reader_->get_next_row(file_row_, has_next))) {
      LOG_WARN("failed to get next row from file table reader", K(ret));
    } else if (!has_next) {
      ret = OB_ITER_END;
    } else if (OB_FAIL(write_file_row_to_columns(file_row_))) {
      LOG_WARN("failed to write file row to columns", K(ret));
    }
  }
  return ret;
}

int ObFunctionTableOp::write_file_row_to_columns(const ObIArray<ObObj> &row)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(MY_SPEC.column_exprs_.count() != row.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file row column count mismatch",
             K(MY_SPEC.column_exprs_.count()), K(row.count()), K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < row.count(); ++i) {
      ObExpr *expr = MY_SPEC.column_exprs_.at(i);
      const ObObj &obj = row.at(i);
      CK (OB_NOT_NULL(expr));
      if (OB_FAIL(ret)) {
      } else if (obj.is_null()) {
        expr->locate_datum_for_write(eval_ctx_).set_null();
      } else {
        const ObObjDatumMapType &datum_map = expr->obj_datum_map_;
        ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
        if (OB_FAIL(datum.from_obj(obj, datum_map))) {
          LOG_WARN("failed to convert obj to datum", K(obj), K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        expr->set_evaluated_projected(eval_ctx_);
      }
    }
  }
  return ret;
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to get plan ctx", K(ret), K(plan_ctx));
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("value expr is not init", K(ret));
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected value", K(ret), K(MY_SPEC.value_expr_->datum_meta_.type_));
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get value table", K(ret));
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("unexpected composite type", K(ret), K(composite->get_type()));
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
            LOG_WARN("adjust lob datum failed", K(ret), K(obj_stack[i].get_meta()), K(expr->obj_meta_));
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to get plan ctx", K(ret), K(plan_ctx));
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
      LOG_WARN("failed to eval value expr", K(ret));
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
