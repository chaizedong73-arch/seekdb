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

#include "sql/engine/file_scan/file_table_reader.h"
#include "sql/engine/file_scan/ob_csv_file_table_reader.h"
#include "sql/engine/file_scan/ob_glob_file_table_reader.h"
#include "sql/engine/file_scan/ob_json_file_table_reader.h"
#include "sql/engine/file_scan/ob_parquet_file_table_reader.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

int parse_file_read_options(const ObIArray<ObObj> &args, FileReadOptions &options)
{
  int ret = OB_SUCCESS;
  if (args.count() < 1 || args.count() % 2 != 1) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (int64_t i = 1; OB_SUCC(ret) && i < args.count(); i += 2) {
    const ObObj &value = args.at(i + 1);
    if (!args.at(i).is_string_type() || value.is_null()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ObString name = args.at(i).get_string();
      for (int64_t j = 1; OB_SUCC(ret) && j < i; j += 2) {
        if (0 == name.case_compare(args.at(j).get_string())) {
          ret = OB_INVALID_ARGUMENT;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (0 == name.case_compare("header")) {
        if (!value.is_integer_type() || (value.get_int() != 0 && value.get_int() != 1)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          options.parse_header_ = value.get_int() != 0;
        }
      } else if (0 == name.case_compare("sample_size")) {
        if (!value.is_integer_type() || value.get_int() != -1) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL sample_size other than -1");
        }
      } else if (!value.is_string_type()) {
        ret = OB_INVALID_ARGUMENT;
      } else if (0 == name.case_compare("delimiter") || 0 == name.case_compare("delim")) {
        options.field_delimiter_ = value.get_string();
        if (options.field_delimiter_.length() != 1) { ret = OB_INVALID_ARGUMENT; }
      } else if (0 == name.case_compare("quote")) {
        options.quote_char_ = value.get_string();
        if (options.quote_char_.length() != 1) { ret = OB_INVALID_ARGUMENT; }
      } else if (0 == name.case_compare("escape")) {
        options.escape_char_ = value.get_string();
        if (options.escape_char_.length() != 1) { ret = OB_INVALID_ARGUMENT; }
      } else if (0 == name.case_compare("nullstr") || 0 == name.case_compare("null")) {
        options.null_str_ = value.get_string();
      } else if (0 == name.case_compare("format")) {
        if (0 != value.get_string().case_compare("newline_delimited")) {
          ret = OB_NOT_SUPPORTED;
        }
      } else {
        ret = OB_INVALID_ARGUMENT;
      }
    }
  }
  return ret;
}

const char *IFileTableReader::format_to_str(FileTableFormat format)
{
  const char *str = "UNKNOWN";
  switch (format) {
    case FILE_TABLE_CSV:
      str = "CSV";
      break;
    case FILE_TABLE_JSON:
      str = "JSON";
      break;
    case FILE_TABLE_NDJSON:
      str = "NDJSON";
      break;
    case FILE_TABLE_PARQUET:
      str = "PARQUET";
      break;
    case FILE_TABLE_GLOB:
      str = "GLOB";
      break;
    default:
      break;
  }
  return str;
}

int create_file_table_reader(const FileTableFormat format, ObIAllocator &alloc,
    IFileTableReader *&reader)
{
  int ret = OB_SUCCESS;
  reader = NULL;
  switch (format) {
    case FILE_TABLE_CSV: {
      void *buf = alloc.alloc(sizeof(CsvFileTableReader));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to alloc csv file table reader", K(ret));
      } else {
        reader = new (buf) CsvFileTableReader(alloc);
      }
      break;
    }
    case FILE_TABLE_GLOB: {
      void *buf = alloc.alloc(sizeof(GlobFileTableReader));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to alloc glob file table reader", K(ret));
      } else {
        reader = new (buf) GlobFileTableReader(alloc);
      }
      break;
    }
    case FILE_TABLE_JSON:
    case FILE_TABLE_NDJSON: {
      void *buf = alloc.alloc(sizeof(JsonFileTableReader));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to alloc json file table reader", K(ret));
      } else {
        reader = new (buf) JsonFileTableReader(alloc, format);
      }
      break;
    }
    case FILE_TABLE_PARQUET: {
      void *buf = alloc.alloc(sizeof(ParquetFileTableReader));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to alloc parquet file table reader", K(ret));
      } else {
        reader = new (buf) ParquetFileTableReader(alloc);
      }
      break;
    }
    default:
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, IFileTableReader::format_to_str(format));
      LOG_WARN("file table format is not supported yet", K(format), K(ret));
      break;
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
