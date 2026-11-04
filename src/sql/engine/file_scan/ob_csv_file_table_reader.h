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
 * @brief Skeleton csv file table reader.
 * Only the column schema is resolved: the first line is read as column names
 * when parse_header_ is on, otherwise generated as COLUMN_0, COLUMN_1, ...
 * Data rows are not produced yet (TODO: full csv data scan).
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
  int parse_header(const char *line, int64_t line_len);

private:
  common::ObIAllocator &alloc_;
  common::ObString path_;
  FileReadOptions options_;
  common::ObSEArray<FileColumnMeta, 16> columns_;
  ::FILE *fp_;
  FileTableFileStat file_stat_;
  bool inited_;
};

} // end namespace sql
} // end namespace oceanbase
