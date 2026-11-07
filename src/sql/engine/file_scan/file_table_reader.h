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

#include <stdint.h>
#include "common/object/ob_object.h"
#include "lib/alloc/ob_iallocator.h"
#include "lib/container/ob_bit_set.h"
#include "lib/container/ob_iarray.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{

/**
 * @brief The file formats supported by "file as a table" (file table) sql.
 */
enum FileTableFormat
{
  FILE_TABLE_CSV = 0,
  FILE_TABLE_JSON = 1,
  FILE_TABLE_NDJSON = 2,
  FILE_TABLE_PARQUET = 3,
  FILE_TABLE_GLOB = 4,
  FILE_TABLE_MAX_FORMAT,
};

constexpr int64_t OB_FILE_TABLE_MAX_COLUMN_COUNT = 4096;
constexpr int64_t OB_FILE_TABLE_MAX_FILE_SIZE = 64LL << 30; // 64GB
constexpr int64_t OB_FILE_TABLE_MAX_FILE_COUNT = 10000;     // max files matched by a glob pattern
constexpr int64_t OB_FILE_TABLE_DEFAULT_SAMPLE_SIZE = -1;

/**
 * @brief Schema meta of one column sampled from a file table.
 */
struct FileColumnMeta
{
  FileColumnMeta() : name_(), type_()
  {}
  common::ObString name_;
  common::ObObjMeta type_;
  TO_STRING_KV(K_(name), K_(type));
};

/**
 * @brief Read options of a file table, parsed from sql options by upper layer.
 */
struct FileReadOptions
{
  FileReadOptions()
      : parse_header_(true),
        field_delimiter_(","),
        line_delimiter_("\n"),
        quote_char_("\""),
        escape_char_("\\"),
        null_str_(),
        sample_size_(OB_FILE_TABLE_DEFAULT_SAMPLE_SIZE),
        max_rows_(-1)
  {}
  bool parse_header_;              // whether the first line of csv is column names
  common::ObString field_delimiter_; // csv field delimiter, default ","
  common::ObString line_delimiter_;  // csv line delimiter, default "\n"
  common::ObString quote_char_;      // csv quote char, default "\""
  common::ObString escape_char_;     // csv escape char, default "\\"
  common::ObString null_str_;        // string literal that represents null value
  common::ObString secure_root_;     // canonical root checked again for glob matches
  int64_t sample_size_;              // only -1 (full schema inference) is supported
  int64_t max_rows_;                 // -1 means unlimited
};

int parse_file_read_options(const common::ObIArray<common::ObObj> &args,
                           FileReadOptions &options);

/**
 * @brief Snapshot of a file used for change detection between rescans.
 */
struct FileTableFileStat
{
  FileTableFileStat() : file_size_(0), last_modify_time_(0), modify_nsec_(0), inode_(0)
  {}
  bool operator==(const FileTableFileStat &other) const
  {
    return file_size_ == other.file_size_ && last_modify_time_ == other.last_modify_time_
        && modify_nsec_ == other.modify_nsec_ && inode_ == other.inode_;
  }
  bool operator!=(const FileTableFileStat &other) const
  {
    return !(*this == other);
  }
  int64_t file_size_;
  int64_t last_modify_time_;
  int64_t modify_nsec_;
  uint64_t inode_;
};

/**
 * @brief Interface for iterating rows of a file table.
 * A reader instance is bound to one path (file or glob pattern) and one format.
 * The instance and all ObString/ObObj it returns are allocated from the allocator
 * given to create_file_table_reader().
 */
class IFileTableReader
{
public:
  IFileTableReader()
  {}
  virtual ~IFileTableReader()
  {}
  // Open the file(s) and resolve the column schema of the file table.
  virtual int init(const common::ObString &path, const FileReadOptions &options) = 0;
  // Output the schema of the file table, must be called after init() succeeds.
  virtual int get_column_metas(common::ObIArray<FileColumnMeta> &columns) const = 0;
  // Fetch the next row. has_next = false means the iteration reaches the end.
  virtual int get_next_row(common::ObIArray<common::ObObj> &row, bool &has_next) = 0;
  // Rewind the iteration back to the first data row, used on rescan when the
  // file is not changed. After reuse(), get_column_metas() should still work.
  virtual int reuse() = 0;
  // Snapshot of file size and last modify time for change detection.
  virtual int get_file_stat(FileTableFileStat &stat) const = 0;
  // Release file handles. The reader can be init() again after close().
  virtual void close() = 0;
  // Select the columns that the caller actually needs (projection pushdown),
  // indexes are the positions in get_column_metas() order. An empty bit set
  // (default state) means all columns are needed. Readers may skip IO and
  // decoding of unselected columns and output NULL values for them; the row
  // width is always the full schema width. Must be called after init() and
  // before get_next_row().
  // NOTE: this virtual method is an interface extension for column pruning
  // (e.g. parquet), the default implementation does nothing so that existing
  // readers keep their behaviour.
  virtual int set_projection(const common::ObBitSet<> &col_idxs)
  {
    UNUSED(col_idxs);
    return OB_SUCCESS;
  }
  static const char *format_to_str(FileTableFormat format);
};

/**
 * @brief Factory: create an empty reader of the given format.
 * The reader is allocated from alloc and still needs init() to open the file.
 */
int create_file_table_reader(
    const FileTableFormat format, common::ObIAllocator &alloc, IFileTableReader *&reader);

} // end namespace sql
} // end namespace oceanbase
