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

#include <stdio.h>

#include "sql/engine/file_scan/file_table_reader.h"
#include "sql/engine/file_scan/ob_file_table_type_infer.h"

namespace oceanbase
{
namespace sql
{

/**
 * @brief Reader for line-delimited json files (one flat json object per line).
 * Both FILE_TABLE_JSON and FILE_TABLE_NDJSON are read with this reader: each
 * line must be a single flat json object (no nested object/array values,
 * scalar values only). During init() the whole file is scanned once to build
 * the union of the fields (columns are ordered by first appearance) and to
 * infer the column types (see ob_file_table_type_infer.h). A row missing some
 * fields outputs null for them. Nested object/array values are rejected with
 * OB_INVALID_DATA (file name, line number and field name are logged), so are
 * type conflicts that cannot be widened (e.g. bool + int in one column).
 */
class JsonFileTableReader : public IFileTableReader
{
public:
  explicit JsonFileTableReader(common::ObIAllocator &alloc, FileTableFormat format);
  virtual ~JsonFileTableReader();

  virtual int init(const common::ObString &path, const FileReadOptions &options) override;
  virtual int get_column_metas(common::ObIArray<FileColumnMeta> &columns) const override;
  virtual int get_next_row(common::ObIArray<common::ObObj> &row, bool &has_next) override;
  virtual int reuse() override;
  virtual int get_file_stat(FileTableFileStat &stat) const override;
  virtual void close() override;

private:
  int dup_string_(const common::ObString &src, common::ObString &dst);
  int grow_buf_(char *&buf, int64_t &cap, int64_t need, int64_t used_len);
  int next_char_(char &c, bool &eof);
  int peek_char_(char &c, bool &eof);
  // read one physical line into line_buf_ (without the trailing \n, a trailing
  // \r is stripped); has_line = false means eof
  int read_line_(bool &has_line);
  // find the column index of a field, add it when not found (init phase only);
  // returns OB_SUCCESS with col_idx = -1 when the columns are frozen
  // (get_next_row phase) and the field is not in the schema
  int find_or_add_column_(const common::ObString &name, int64_t &col_idx);
  // scan the whole file once to build the field union and infer types
  int infer_schema_();
  // parse the current line (line_buf_) as one flat json object; every parsed
  // key/value pair is reported through handle_kv_()
  int parse_json_line_(bool for_infer, bool &is_blank);
  // parse a json string literal starting at pos, the decoded value is appended
  // to val_buf_; text points into val_buf_ (for decoded strings) or line_buf_
  // (for raw tokens)
  int parse_json_string_(int64_t &pos, common::ObString &text);
  int handle_kv_(bool for_infer, int64_t line_no, const common::ObString &name,
      const common::ObString &text, FileCellKind kind);
  void clear_line_state_();

private:
  static const int64_t READ_BUF_SIZE = 1LL << 20;    // 1MB stream buffer
  static const int64_t INIT_LINE_BUF_SIZE = 1LL << 17;
  static const int64_t MAX_LINE_BUF_SIZE = 256LL << 20;

private:
  common::ObIAllocator &alloc_;
  FileTableFormat format_;
  common::ObString path_;
  FileReadOptions options_;
  common::ObSEArray<FileColumnMeta, 16> columns_;
  common::ObSEArray<FileCellKind, 16> cell_kinds_;
  ::FILE *fp_;
  char *read_buf_;
  int64_t buf_len_;
  int64_t buf_pos_;
  char *line_buf_;
  int64_t line_cap_;
  int64_t line_used_;
  char *val_buf_;
  int64_t val_cap_;
  int64_t val_used_;
  common::ObSEArray<bool, 16> row_set_flags_;
  common::ObSEArray<common::ObObj, 16> row_objs_;
  // 2026-11-07: DESCRIBE null 列口径——采样行中缺失/为 NULL 的列可空
  int64_t infer_rows_;
  common::ObSEArray<int64_t, 16> col_present_;
  common::ObSEArray<bool, 16> col_saw_null_;
  common::ObSEArray<bool, 16> infer_touched_;
  int64_t line_no_;
  int64_t rows_emitted_;
  FileTableFileStat file_stat_;
  bool inited_;
};

} // end namespace sql
} // end namespace oceanbase
