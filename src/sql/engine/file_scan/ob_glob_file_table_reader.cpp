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

#include "sql/engine/file_scan/ob_glob_file_table_reader.h"
#include "lib/charset/ob_charset.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{
int dup_string(ObIAllocator &alloc, const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.length() > INT32_MAX)) {
    ret = OB_SIZE_OVERFLOW;
  } else if (src.empty()) {
    dst = src;
  } else {
    char *buf = static_cast<char *>(alloc.alloc(src.length()));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to alloc memory for string", K(src.length()), K(ret));
    } else {
      MEMCPY(buf, src.ptr(), src.length());
      dst.assign_ptr(buf, static_cast<int32_t>(src.length()));
    }
  }
  return ret;
}
} // end anonymous namespace

GlobFileTableReader::GlobFileTableReader(ObIAllocator &alloc) : alloc_(alloc), inited_(false)
{}

GlobFileTableReader::~GlobFileTableReader()
{
  close();
}

int GlobFileTableReader::init(const ObString &path, const FileReadOptions &options)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(path.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("glob pattern is empty", K(ret));
  } else if (OB_FAIL(dup_string(alloc_, path, path_))) {
    LOG_WARN("failed to copy glob pattern", K(ret));
  } else {
    inited_ = true;
  }
  if (OB_SUCC(ret)) {
    // fixed schema: one "file" column containing matched file paths
    FileColumnMeta col;
    col.name_ = ObString("file");
    col.type_.set_varchar();
    col.type_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (OB_FAIL(columns_.push_back(col))) {
      LOG_WARN("failed to push file column meta", K(ret));
    }
  }
  return ret;
}

int GlobFileTableReader::get_column_metas(ObIArray<FileColumnMeta> &columns) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("glob file table reader is not inited", K(ret));
  } else if (OB_FAIL(columns.assign(columns_))) {
    LOG_WARN("failed to assign column metas", K(ret));
  }
  return ret;
}

int GlobFileTableReader::get_next_row(ObIArray<ObObj> &row, bool &has_next)
{
  int ret = OB_SUCCESS;
  has_next = false;
  row.reuse();
  // TODO: match the glob pattern and output matched file paths.
  return ret;
}

int GlobFileTableReader::reuse()
{
  // nothing to rewind: no row is produced yet
  return OB_SUCCESS;
}

int GlobFileTableReader::get_file_stat(FileTableFileStat &stat) const
{
  // a glob pattern has no meaningful single file stat
  stat = FileTableFileStat();
  return OB_SUCCESS;
}

void GlobFileTableReader::close()
{
  inited_ = false;
}

} // end namespace sql
} // end namespace oceanbase
