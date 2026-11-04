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

namespace oceanbase
{
using namespace common;
namespace sql
{

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
