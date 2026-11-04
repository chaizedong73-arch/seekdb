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

#pragma once

#include "sql/engine/expr/ob_expr_operator.h"
#include "sql/engine/file_scan/file_table_reader.h"

namespace oceanbase
{
namespace sql
{

/**
 * @brief Base class of file table functions, such as read_csv('a.csv') and
 * glob('*.csv'). These functions are only used in FROM clause as function
 * tables: each file table function outputs a table whose schema is sampled
 * from the file(s) instead of a single scalar value.
 * The scalar evaluation is not supported, the actual rows are produced by
 * ObFunctionTableOp with the file scan reader.
 */
class FileTableFuncExpr : public ObFuncExprOperator
{
public:
  FileTableFuncExpr(common::ObIAllocator &alloc, ObExprOperatorType type, const char *name);
  virtual ~FileTableFuncExpr();

  virtual int calc_result_type1(ObExprResType &type,
                                ObExprResType &path,
                                common::ObExprTypeCtx &type_ctx) const;
  // the path argument must be a constant string, no runtime scalar evaluation
  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx,
                      const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;

  static int eval_not_supported(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res_datum);
  static bool is_file_table_func_type(ObExprOperatorType type);
  // returns the file table format of the given operator type,
  // FILE_TABLE_MAX_FORMAT is returned when the type is not a file table function
  static FileTableFormat get_format_by_expr_type(ObExprOperatorType type);

private:
  DISALLOW_COPY_AND_ASSIGN(FileTableFuncExpr);
};

#define DEF_FILE_TABLE_FUNC_EXPR_CLASS(ClassName, ExprType, FuncName)                        \
  class ClassName : public FileTableFuncExpr                                                 \
  {                                                                                          \
  public:                                                                                    \
    explicit ClassName(common::ObIAllocator &alloc)                                          \
        : FileTableFuncExpr(alloc, ExprType, FuncName)                                       \
    {}                                                                                       \
    virtual ~ClassName()                                                                     \
    {}                                                                                       \
                                                                                             \
  private:                                                                                   \
    DISALLOW_COPY_AND_ASSIGN(ClassName);                                                     \
  };

DEF_FILE_TABLE_FUNC_EXPR_CLASS(ReadCsvFuncExpr, T_FUN_SYS_READ_CSV, "read_csv")
DEF_FILE_TABLE_FUNC_EXPR_CLASS(ReadJsonFuncExpr, T_FUN_SYS_READ_JSON, "read_json")
DEF_FILE_TABLE_FUNC_EXPR_CLASS(ReadNdjsonFuncExpr, T_FUN_SYS_READ_NDJSON, "read_ndjson")
DEF_FILE_TABLE_FUNC_EXPR_CLASS(ReadParquetFuncExpr, T_FUN_SYS_READ_PARQUET, "read_parquet")
DEF_FILE_TABLE_FUNC_EXPR_CLASS(GlobFuncExpr, T_FUN_SYS_GLOB, "glob")

#undef DEF_FILE_TABLE_FUNC_EXPR_CLASS

} /* namespace sql */
} /* namespace oceanbase */
