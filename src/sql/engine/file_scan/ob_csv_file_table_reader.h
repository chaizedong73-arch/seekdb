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

namespace oceanbase
{
namespace sql
{

/**
 * @brief Streaming csv file table reader.
 * The file is parsed with a fixed read buffer, rows are never fully
 * materialized in memory: one logical row (a quoted field may span the read
 * buffer boundary and contain line delimiters) is decoded field by field into
 * a growable value buffer.
 * Schema resolution: when parse_header_ is on the first logical row is the
 * column names, otherwise columns are generated as COLUMN_0, COLUMN_1, ...
 * The whole file (sampled rows) is scanned once during init() to infer the
 * column types deterministically (see ob_file_table_type_infer.h).
 * Bad rows (field count mismatch, unterminated quote, cell conversion error)
 * are rejected with OB_INVALID_DATA and the file name plus line number in the
 * log.
 */
class CsvFileTableReader : public IFileTableReader
{
public:
  explicit CsvFileTableReader(common::ObIAllocator &alloc);
  virtual ~CsvFileTableReader();

  virtual int init(const common::ObString &path, const FileReadOptions &options) override;
  virtual int get_column_metas(common::ObIArray<FileColumnMeta> &columns) const override;
  virtual int get_next_row(common::ObIArray<common::ObObj> &row, bool &has_next) override;
  virtual int reuse() override;
  virtual int get_file_stat(FileTableFileStat &stat) const override;
  virtual void close() override;

private:
  int dup_string_(const common::ObString &src, common::ObString &dst);
  // grow a heap buffer allocated from alloc_ to at least "need" bytes
  int grow_buf_(char *&buf, int64_t &cap, int64_t need);
  // buffered stream primitives over fp_
  int next_char_(char &c, bool &eof);
  int peek_char_(char &c, bool &eof);
  // append one byte to the value buffer of the current row
  int append_byte_(char c);
  // close the current field: [field_start, val_buf_used_) is one cell text
  int finish_field_(int64_t field_start, bool quoted);
  // parse one logical csv row into fields_ (may span \n inside quotes);
  // has_row = false means eof is reached with no pending row. Empty rows
  // (no character except the line delimiter) are reported as has_row = false
  // so that callers skip them.
  int read_csv_row_(bool &has_row);
  // rewind and skip the header row if parse_header_ is on
  int rewind_to_data_();
  // scan the sampled data rows to infer the column types
  int infer_schema_();
  // classify one csv cell text of the current row
  int classify_csv_cell_(bool quoted, const common::ObString &text, FileCellKind &kind) const;
  void clear_row_state_();

private:
  static const int64_t READ_BUF_SIZE = 1LL << 20;     // 1MB stream buffer
  static const int64_t INIT_VAL_BUF_SIZE = 1LL << 17; // 128KB initial row buffer
  static const int64_t MAX_ROW_BUF_SIZE = 256LL << 20;

private:
  common::ObIAllocator &alloc_;
  common::ObString path_;
  FileReadOptions options_;
  char field_delim_ch_;
  char quote_ch_;
  char escape_ch_;
  char line_delim_ch_;
  common::ObSEArray<FileColumnMeta, 16> columns_;
  common::ObSEArray<FileCellKind, 16> cell_kinds_;
  ::FILE *fp_;
  char *read_buf_;
  int64_t buf_len_;
  int64_t buf_pos_;
  char *val_buf_;
  int64_t val_buf_cap_;
  int64_t val_buf_used_;
  common::ObSEArray<common::ObString, 16> fields_;
  common::ObSEArray<bool, 16> field_quoted_;
  int64_t data_line_no_;
  int64_t rows_emitted_;
  FileTableFileStat file_stat_;
  bool inited_;
};

} // end namespace sql
} // end namespace oceanbase
