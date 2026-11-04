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

namespace oceanbase
{
using namespace common;
namespace sql
{

FileTableFuncExpr::FileTableFuncExpr(ObIAllocator &alloc, ObExprOperatorType type, const char *name)
    : ObFuncExprOperator(alloc, type, name, 1, NOT_VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
{}

FileTableFuncExpr::~FileTableFuncExpr()
{}

int FileTableFuncExpr::calc_result_type1(ObExprResType &type,
                                         ObExprResType &path,
                                         ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  path.set_calc_type(ObVarcharType);
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
  if (OB_UNLIKELY(1 != raw_expr.get_param_count())) {
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
