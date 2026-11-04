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

#include "sql/engine/file_scan/ob_csv_file_table_reader.h"
#include <stdio.h>
#include "lib/charset/ob_charset.h"
#include "lib/ob_define.h"
#include "lib/utility/alloc_assist.h"
#include "lib/utility/ob_print_utils.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{
constexpr int64_t MAX_HEADER_LINE_LEN = 16 * 1024; // max bytes of one header line

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

CsvFileTableReader::CsvFileTableReader(ObIAllocator &alloc)
    : alloc_(alloc), fp_(NULL), inited_(false)
{}

CsvFileTableReader::~CsvFileTableReader()
{
  close();
}

int CsvFileTableReader::init(const ObString &path, const FileReadOptions &options)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(path.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("file table path is empty", K(ret));
  } else if (OB_FAIL(dup_string(alloc_, options.field_delimiter_, options_.field_delimiter_))) {
    LOG_WARN("failed to copy field delimiter", K(ret));
  } else if (OB_FAIL(dup_string(alloc_, options.line_delimiter_, options_.line_delimiter_))) {
    LOG_WARN("failed to copy line delimiter", K(ret));
  } else if (OB_FAIL(dup_string(alloc_, options.quote_char_, options_.quote_char_))) {
    LOG_WARN("failed to copy quote char", K(ret));
  } else if (OB_FAIL(dup_string(alloc_, options.escape_char_, options_.escape_char_))) {
    LOG_WARN("failed to copy escape char", K(ret));
  } else if (OB_FAIL(dup_string(alloc_, options.null_str_, options_.null_str_))) {
    LOG_WARN("failed to copy null string", K(ret));
  } else {
    options_.parse_header_ = options.parse_header_;
    options_.sample_size_ = options.sample_size_;
    options_.max_rows_ = options.max_rows_;
    path_ = path;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(fp_ = ::fopen(path.ptr(), "rb"))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_USER_ERROR(OB_FILE_NOT_EXIST, path.length(), path.ptr());
    LOG_WARN("failed to open csv file", K(path), K(ret));
  } else if (OB_FAIL(get_file_stat(file_stat_))) {
    LOG_WARN("failed to get file stat", K(path), K(ret));
  } else {
    inited_ = true;
  }
  if (OB_SUCC(ret)) {
    // resolve column schema: read the first line as header
    char buf[MAX_HEADER_LINE_LEN];
    if (OB_ISNULL(::fgets(buf, static_cast<int>(sizeof(buf)), fp_))) {
      // empty file: produce one default column
      FileColumnMeta col;
      col.name_ = ObString("COLUMN_0");
      col.type_.set_varchar();
      col.type_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      if (OB_FAIL(columns_.push_back(col))) {
        LOG_WARN("failed to push default column", K(ret));
      }
    } else if (OB_FAIL(parse_header(buf, static_cast<int64_t>(STRLEN(buf))))) {
      LOG_WARN("failed to parse csv header", K(path), K(ret));
    }
  }
  if (OB_FAIL(ret)) {
    close();
  }
  return ret;
}

int CsvFileTableReader::parse_header(const char *line, const int64_t line_len)
{
  int ret = OB_SUCCESS;
  const char delimiter =
      (options_.field_delimiter_.empty() ? ',' : options_.field_delimiter_.ptr()[0]);
  const char quote_ch = (options_.quote_char_.empty() ? '"' : options_.quote_char_.ptr()[0]);
  int64_t col_start = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i <= line_len; ++i) {
    if (i != line_len && line[i] != delimiter && line[i] != '\n' && line[i] != '\r') {
      // continue scanning current column
    } else {
      int64_t start = col_start;
      int64_t end = i;
      // trim spaces and quote chars around the column name
      while (start < end && (line[start] == ' ' || line[start] == quote_ch)) {
        ++start;
      }
      while (end > start
             && (line[end - 1] == ' ' || line[end - 1] == '\r' || line[end - 1] == quote_ch)) {
        --end;
      }
      FileColumnMeta col;
      col.type_.set_varchar();
      col.type_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      if (end == start) { // empty column name: generate COLUMN_%ld
        char default_name[32];
        int64_t pos = 0;
        if (OB_FAIL(databuff_printf(default_name, sizeof(default_name), pos, "COLUMN_%ld",
                static_cast<int64_t>(columns_.count())))) {
          LOG_WARN("failed to build default column name", K(ret));
        } else if (OB_FAIL(dup_string(alloc_, ObString(static_cast<int32_t>(pos), default_name),
                           col.name_))) {
          LOG_WARN("failed to copy default column name", K(ret));
        }
      } else if (OB_FAIL(dup_string(alloc_, ObString(static_cast<int32_t>(end - start),
                           line + start),
                           col.name_))) {
        LOG_WARN("failed to copy column name", K(ret));
      }
      if (OB_FAIL(ret)) {
      } else if (columns_.count() >= OB_FILE_TABLE_MAX_COLUMN_COUNT) {
        ret = OB_SIZE_OVERFLOW;
        LOG_WARN("too many columns in csv file", K(columns_.count()), K(ret));
      } else if (OB_FAIL(columns_.push_back(col))) {
        LOG_WARN("failed to push column meta", K(ret));
      }
      col_start = i + 1;
    }
  }
  return ret;
}

int CsvFileTableReader::get_column_metas(ObIArray<FileColumnMeta> &columns) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("csv file table reader is not inited", K(ret));
  } else if (OB_FAIL(columns.assign(columns_))) {
    LOG_WARN("failed to assign column metas", K(ret));
  }
  return ret;
}

int CsvFileTableReader::get_next_row(ObIArray<ObObj> &row, bool &has_next)
{
  int ret = OB_SUCCESS;
  has_next = false;
  row.reuse();
  // TODO: read and parse csv data rows.
  return ret;
}

int CsvFileTableReader::reuse()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(fp_)) {
    ::rewind(fp_);
    // skip the header line again if it exists
    if (options_.parse_header_) {
      char buf[MAX_HEADER_LINE_LEN];
      (void)::fgets(buf, static_cast<int>(sizeof(buf)), fp_);
    }
  }
  return ret;
}

int CsvFileTableReader::get_file_stat(FileTableFileStat &stat) const
{
  int ret = OB_SUCCESS;
  stat = FileTableFileStat();
  if (OB_UNLIKELY(path_.empty())) {
    // not inited yet, return empty stat
  } else {
#ifdef _WIN32
    struct __stat64 st;
    if (0 != ::_stat64(path_.ptr(), &st)) {
#else
    struct stat st;
    if (0 != ::stat(path_.ptr(), &st)) {
#endif
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to stat csv file", K(path_), K(ret));
    } else {
      stat.file_size_ = static_cast<int64_t>(st.st_size);
      stat.last_modify_time_ = static_cast<int64_t>(st.st_mtime);
    }
  }
  return ret;
}

void CsvFileTableReader::close()
{
  if (OB_NOT_NULL(fp_)) {
    ::fclose(fp_);
    fp_ = NULL;
  }
  inited_ = false;
}

} // end namespace sql
} // end namespace oceanbase
