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

#include "sql/engine/file_scan/file_table_reader.h"

namespace oceanbase
{
namespace sql
{

/**
 * @brief Skeleton reader for glob patterns ("*.csv" style paths).
 * The file table has one fixed column named "file" (VARCHAR), whose values
 * are the matched file paths. Data rows are not produced yet
 * (TODO: glob matching and directory traversal).
 */
class GlobFileTableReader : public IFileTableReader
{
public:
  explicit GlobFileTableReader(common::ObIAllocator &alloc);
  virtual ~GlobFileTableReader();

  virtual int init(const common::ObString &path, const FileReadOptions &options) override;
  virtual int get_column_metas(common::ObIArray<FileColumnMeta> &columns) const override;
  virtual int get_next_row(common::ObIArray<common::ObObj> &row, bool &has_next) override;
  virtual int reuse() override;
  virtual int get_file_stat(FileTableFileStat &stat) const override;
  virtual void close() override;

private:
  common::ObIAllocator &alloc_;
  common::ObString path_;
  common::ObSEArray<FileColumnMeta, 1> columns_;
  bool inited_;
};

} // end namespace sql
} // end namespace oceanbase
