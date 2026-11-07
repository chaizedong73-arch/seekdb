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
#include "share/ob_errno.h"

#include <stdio.h>
#include <string.h>

#include "lib/charset/ob_charset.h"
#include "lib/ob_define.h"
#include "lib/string/ob_sql_string.h"
#include "lib/utility/alloc_assist.h"
#include "lib/utility/ob_print_utils.h"
#include "sql/engine/file_scan/ob_file_table_type_infer.h"

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

CsvFileTableReader::CsvFileTableReader(ObIAllocator &alloc)
    : alloc_(alloc),
      field_delim_ch_(','),
      quote_ch_('"'),
      escape_ch_('\\'),
      line_delim_ch_('\n'),
      fp_(NULL),
      read_buf_(NULL),
      buf_len_(0),
      buf_pos_(0),
      val_buf_(NULL),
      val_buf_cap_(0),
      val_buf_used_(0),
      data_line_no_(0),
      rows_emitted_(0),
      inited_(false)
{}

CsvFileTableReader::~CsvFileTableReader()
{
  close();
}

int CsvFileTableReader::dup_string_(const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.length() > INT32_MAX)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("string too long", K(src.length()), K(ret));
  } else if (src.empty()) {
    dst = src;
  } else {
    char *buf = static_cast<char *>(alloc_.alloc(src.length() + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to alloc memory for string", K(src.length()), K(ret));
    } else {
      MEMCPY(buf, src.ptr(), static_cast<size_t>(src.length()));
      buf[src.length()] = '\0';
      dst.assign_ptr(buf, static_cast<int32_t>(src.length()));
    }
  }
  return ret;
}

int CsvFileTableReader::grow_buf_(char *&buf, int64_t &cap, int64_t need)
{
  int ret = OB_SUCCESS;
  if (need <= cap) {
    // fast path
  } else {
    int64_t new_cap = (cap <= 0 ? INIT_VAL_BUF_SIZE : cap);
    while (new_cap < need) {
      new_cap *= 2;
    }
    if (OB_UNLIKELY(new_cap > MAX_ROW_BUF_SIZE)) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("file table row buffer too large", K(new_cap), K(need), K(ret));
    } else {
      char *new_buf = static_cast<char *>(alloc_.alloc(new_cap));
      if (OB_ISNULL(new_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to alloc row buffer", K(new_cap), K(ret));
      } else if (OB_NOT_NULL(buf) && val_buf_used_ > 0) {
        MEMCPY(new_buf, buf, static_cast<size_t>(val_buf_used_));
      }
      if (OB_SUCC(ret)) {
        if (OB_NOT_NULL(buf) && cap > 0) {
          alloc_.free(buf);
        }
        buf = new_buf;
        cap = new_cap;
      } else if (OB_NOT_NULL(new_buf)) {
        alloc_.free(new_buf);
      }
    }
  }
  return ret;
}

int CsvFileTableReader::next_char_(char &c, bool &eof)
{
  int ret = OB_SUCCESS;
  eof = false;
  if (buf_pos_ >= buf_len_) {
    buf_len_ = ::fread(read_buf_, 1, static_cast<size_t>(READ_BUF_SIZE), fp_);
    buf_pos_ = 0;
    if (0 == buf_len_) {
      if (0 != ::ferror(fp_)) {
        ret = OB_IO_ERROR;
        LOG_WARN("failed to read csv file", K(path_), K(ret));
      } else {
        eof = true;
      }
    }
  }
  if (OB_SUCC(ret) && !eof) {
    c = read_buf_[buf_pos_++];
  }
  return ret;
}

int CsvFileTableReader::peek_char_(char &c, bool &eof)
{
  int ret = OB_SUCCESS;
  eof = false;
  if (buf_pos_ >= buf_len_) {
    buf_len_ = ::fread(read_buf_, 1, static_cast<size_t>(READ_BUF_SIZE), fp_);
    buf_pos_ = 0;
    if (0 == buf_len_) {
      if (0 != ::ferror(fp_)) {
        ret = OB_IO_ERROR;
        LOG_WARN("failed to read csv file", K(path_), K(ret));
      } else {
        eof = true;
      }
    }
  }
  if (OB_SUCC(ret) && !eof) {
    c = read_buf_[buf_pos_];
  }
  return ret;
}

int CsvFileTableReader::append_byte_(char c)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(grow_buf_(val_buf_, val_buf_cap_, val_buf_used_ + 1))) {
    LOG_WARN("failed to grow row value buffer", K(val_buf_used_), K(ret));
  } else {
    val_buf_[val_buf_used_++] = c;
  }
  return ret;
}

int CsvFileTableReader::finish_field_(int64_t field_start, bool quoted)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(fields_.count() >= OB_FILE_TABLE_MAX_COLUMN_COUNT)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("too many columns in one csv row", K(fields_.count()), K(ret));
  } else {
    ObString field(static_cast<int32_t>(val_buf_used_ - field_start), val_buf_ + field_start);
    if (OB_FAIL(fields_.push_back(field))) {
      LOG_WARN("failed to push csv field", K(ret));
    } else if (OB_FAIL(field_quoted_.push_back(quoted))) {
      LOG_WARN("failed to push csv field flag", K(ret));
    }
  }
  return ret;
}

void CsvFileTableReader::clear_row_state_()
{
  fields_.reuse();
  field_quoted_.reuse();
  val_buf_used_ = 0;
}

int CsvFileTableReader::read_csv_row_(bool &has_row)
{
  int ret = OB_SUCCESS;
  clear_row_state_();
  has_row = false;
  bool row_has_content = false;
  bool in_quotes = false;
  bool escaped = false;
  bool field_quoted = false;
  int64_t field_start = 0;
  bool row_done = false;
  while (OB_SUCC(ret) && !row_done) {
    char c = '\0';
    bool eof = false;
    if (OB_FAIL(next_char_(c, eof))) {
      LOG_WARN("failed to read csv stream", K(path_), K(ret));
    } else if (eof) {
      if (in_quotes) {
        ret = OB_INVALID_DATA;
        LOG_USER_ERROR(OB_INVALID_DATA);
        LOG_WARN("unterminated quote at the end of csv file", K(path_), K(data_line_no_), K(ret));
      } else {
        if (row_has_content) {
          if (OB_FAIL(finish_field_(field_start, field_quoted))) {
            LOG_WARN("failed to finish last csv field", K(ret));
          } else {
            has_row = true;
          }
        }
        row_done = true;
      }
    } else if (escaped) {
      escaped = false;
      row_has_content = true;
      if (OB_FAIL(append_byte_(c))) {
        LOG_WARN("failed to append escaped char", K(ret));
      }
    } else if (in_quotes) {
      if (quote_ch_ == c) {
        // 2026-11-08: 标准 CSV 语义——引号内连续两个引号表示一个字面引号。
        // 原实现误判为 quote_ch_ == escape_ch_（默认 escape 是 \ 而 quote 是 "，恒不成立），
        // 导致 "" 被当成"结束引号+重开引号"，字面引号丢失。
        char n = '\0';
        bool eof2 = false;
        if (OB_FAIL(peek_char_(n, eof2))) {
          LOG_WARN("failed to peek csv stream", K(path_), K(ret));
        } else if (!eof2 && quote_ch_ == n) {
          char dummy = '\0';
          bool eof3 = false;
          if (OB_FAIL(next_char_(dummy, eof3))) {
            LOG_WARN("failed to consume doubled quote", K(ret));
          } else if (OB_FAIL(append_byte_(quote_ch_))) {
            LOG_WARN("failed to append escaped quote", K(ret));
          }
        } else {
          in_quotes = false;
        }
      } else if (escape_ch_ == c) {
        escaped = true;
      } else {
        if (OB_FAIL(append_byte_(c))) {
          LOG_WARN("failed to append quoted char", K(ret));
        }
      }
    } else if (quote_ch_ == c) {
      in_quotes = true;
      field_quoted = true;
      row_has_content = true;
    } else if (escape_ch_ == c) {
      escaped = true;
      row_has_content = true;
    } else if (field_delim_ch_ == c) {
      row_has_content = true;
      if (OB_FAIL(finish_field_(field_start, field_quoted))) {
        LOG_WARN("failed to finish csv field", K(ret));
      } else {
        field_start = val_buf_used_;
        field_quoted = false;
      }
    } else if ('\r' == c && '\n' == line_delim_ch_) {
      // CR of a CRLF pair: end the row and drop the CR
      char n = '\0';
      bool eof2 = false;
      if (OB_FAIL(peek_char_(n, eof2))) {
        LOG_WARN("failed to peek csv stream", K(path_), K(ret));
      } else if (!eof2 && '\n' == n) {
        char dummy = '\0';
        bool eof3 = false;
        if (OB_FAIL(next_char_(dummy, eof3))) {
          LOG_WARN("failed to consume LF of CRLF", K(ret));
        } else {
          if (row_has_content) {
            if (OB_FAIL(finish_field_(field_start, field_quoted))) {
              LOG_WARN("failed to finish csv field", K(ret));
            } else {
              has_row = true;
            }
          }
          row_done = true;
        }
      } else {
        // lone CR is a literal
        row_has_content = true;
        if (OB_FAIL(append_byte_(c))) {
          LOG_WARN("failed to append char", K(ret));
        }
      }
    } else if (line_delim_ch_ == c) {
      if (row_has_content) {
        if (OB_FAIL(finish_field_(field_start, field_quoted))) {
          LOG_WARN("failed to finish csv field", K(ret));
        } else {
          has_row = true;
        }
      }
      row_done = true;
    } else {
      row_has_content = true;
      if (OB_FAIL(append_byte_(c))) {
        LOG_WARN("failed to append char", K(ret));
      }
    }
  }
  return ret;
}

int CsvFileTableReader::rewind_to_data_()
{
  int ret = OB_SUCCESS;
  buf_len_ = 0;
  buf_pos_ = 0;
  data_line_no_ = 0;
  clear_row_state_();
  ::rewind(fp_);
  if (options_.parse_header_) {
    bool has_row = false;
    if (OB_FAIL(read_csv_row_(has_row))) {
      LOG_WARN("failed to skip csv header row", K(path_), K(ret));
    }
  }
  return ret;
}

int CsvFileTableReader::classify_csv_cell_(bool quoted, const ObString &text,
    FileCellKind &kind) const
{
  int ret = OB_SUCCESS;
  if (text.empty() && !quoted) {
    // empty unquoted field is always null
    kind = FCK_NULL;
  } else if (!options_.null_str_.empty() && options_.null_str_.length() == text.length()
             && 0 == MEMCMP(text.ptr(), options_.null_str_.ptr(),
                        static_cast<size_t>(text.length()))) {
    kind = FCK_NULL;
  } else if (quoted && text.empty()) {
    // a quoted empty string is an empty varchar, not null
    kind = FCK_STRING;
  } else if (OB_FAIL(classify_file_cell_text(text, ObString(), kind))) {
    LOG_WARN("failed to classify csv cell text", K(text), K(ret));
  }
  return ret;
}

int CsvFileTableReader::init(const ObString &path, const FileReadOptions &options)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(path.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("file table path is empty", K(ret));
  } else if (OB_FAIL(dup_string_(path, path_))) {
    LOG_WARN("failed to copy csv path", K(ret));
  } else if (OB_FAIL(dup_string_(options.field_delimiter_, options_.field_delimiter_))) {
    LOG_WARN("failed to copy field delimiter", K(ret));
  } else if (OB_FAIL(dup_string_(options.line_delimiter_, options_.line_delimiter_))) {
    LOG_WARN("failed to copy line delimiter", K(ret));
  } else if (OB_FAIL(dup_string_(options.quote_char_, options_.quote_char_))) {
    LOG_WARN("failed to copy quote char", K(ret));
  } else if (OB_FAIL(dup_string_(options.escape_char_, options_.escape_char_))) {
    LOG_WARN("failed to copy escape char", K(ret));
  } else if (OB_FAIL(dup_string_(options.null_str_, options_.null_str_))) {
    LOG_WARN("failed to copy null string", K(ret));
  } else {
    options_.parse_header_ = options.parse_header_;
    options_.sample_size_ = options.sample_size_;
    options_.max_rows_ = options.max_rows_;
    // single char delimiters: use the first char of the configured strings
    field_delim_ch_ = options_.field_delimiter_.empty() ? ',' : options_.field_delimiter_.ptr()[0];
    quote_ch_ = options_.quote_char_.empty() ? '"' : options_.quote_char_.ptr()[0];
    escape_ch_ = options_.escape_char_.empty() ? '\\' : options_.escape_char_.ptr()[0];
    line_delim_ch_ = options_.line_delimiter_.empty() ? '\n' : options_.line_delimiter_.ptr()[0];
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(read_buf_ = static_cast<char *>(alloc_.alloc(READ_BUF_SIZE)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to alloc csv read buffer", K(ret));
  } else if (OB_ISNULL(fp_ = ::fopen(path_.ptr(), "rb"))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_USER_ERROR(OB_FILE_NOT_EXIST, path_.length(), path_.ptr());
    LOG_WARN("failed to open csv file", K(path_), K(ret));
  } else if (OB_FAIL(get_file_stat(file_stat_))) {
    LOG_WARN("failed to get file stat", K(path_), K(ret));
  } else if (OB_UNLIKELY(file_stat_.file_size_ > OB_FILE_TABLE_MAX_FILE_SIZE)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("csv file is too large", K(path_), K(file_stat_.file_size_), K(ret));
  } else {
    inited_ = true;
  }
  if (OB_SUCC(ret)) {
    // resolve the column schema (names + inferred types) and rewind
    if (OB_FAIL(infer_schema_())) {
      LOG_WARN("failed to infer csv schema", K(path_), K(ret));
    } else if (OB_FAIL(rewind_to_data_())) {
      LOG_WARN("failed to rewind csv file", K(path_), K(ret));
    }
  }
  if (OB_FAIL(ret)) {
    close();
  }
  return ret;
}

int CsvFileTableReader::infer_schema_()
{
  int ret = OB_SUCCESS;
  FileCellKind merged[OB_FILE_TABLE_MAX_COLUMN_COUNT];
  bool saw_null[OB_FILE_TABLE_MAX_COLUMN_COUNT];
  int64_t col_count = 0;
  data_line_no_ = 0;
  buf_len_ = 0;
  buf_pos_ = 0;
  clear_row_state_();
  ::rewind(fp_);
  const bool unlimited = (options_.sample_size_ <= 0);
  const int64_t sample_limit = unlimited ? INT64_MAX : options_.sample_size_;
  int64_t sampled = 0;
  bool header_resolved = false;
  while (OB_SUCC(ret)) {
    bool has_row = false;
    if (OB_FAIL(read_csv_row_(has_row))) {
      LOG_WARN("failed to read csv row for schema inference", K(path_), K(ret));
    } else if (!has_row) {
      // empty row or eof: stop at eof, otherwise skip the empty row
      char c = '\0';
      bool eof_now = false;
      if (OB_FAIL(peek_char_(c, eof_now))) {
        LOG_WARN("failed to peek csv stream", K(ret));
      } else if (eof_now) {
        break;
      }
    } else if (!header_resolved) {
      // first logical row: resolve the column names (the header row itself
      // never participates in type inference)
      col_count = fields_.count();
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
        merged[i] = FCK_NONE;
        saw_null[i] = false;
        FileColumnMeta col;
        if (options_.parse_header_) {
          const ObString &name = fields_.at(i);
          if (!name.empty()) {
            if (OB_FAIL(dup_string_(name, col.name_))) {
              LOG_WARN("failed to copy csv column name", K(ret));
            }
          } else {
            char default_name[32];
            int64_t pos = 0;
            if (OB_FAIL(databuff_printf(default_name, sizeof(default_name), pos, "COLUMN_%ld", i))) {
              LOG_WARN("failed to build default column name", K(ret));
            } else if (OB_FAIL(dup_string_(ObString(static_cast<int32_t>(pos), default_name),
                           col.name_))) {
              LOG_WARN("failed to copy default column name", K(ret));
            }
          }
        } else {
          char default_name[32];
          int64_t pos = 0;
          if (OB_FAIL(databuff_printf(default_name, sizeof(default_name), pos, "COLUMN_%ld", i))) {
            LOG_WARN("failed to build default column name", K(ret));
          } else if (OB_FAIL(dup_string_(ObString(static_cast<int32_t>(pos), default_name),
                         col.name_))) {
            LOG_WARN("failed to copy default column name", K(ret));
          }
        }
        const ObString original_name = col.name_;
        int64_t suffix = 2;
        bool duplicate = true;
        while (OB_SUCC(ret) && duplicate) {
          duplicate = false;
          for (int64_t j = 0; j < columns_.count() && !duplicate; ++j) {
            duplicate = columns_.at(j).name_.case_compare(col.name_) == 0;
          }
          if (duplicate) {
            ObSqlString unique_name;
            if (OB_FAIL(unique_name.append_fmt("%.*s__%ld", original_name.length(), original_name.ptr(), suffix++))) {
            } else if (OB_FAIL(dup_string_(unique_name.string(), col.name_))) {
            }
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(columns_.push_back(col))) {
          LOG_WARN("failed to push csv column meta", K(ret));
        }
      }
      header_resolved = true;
      if (OB_SUCC(ret) && !options_.parse_header_) {
        // without a header the first row is a data row and takes part in the
        // type inference
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
          FileCellKind kind = FCK_NONE;
          if (OB_FAIL(classify_csv_cell_(field_quoted_.at(i), fields_.at(i), kind))) {
            LOG_WARN("failed to classify csv cell", K(i), K(ret));
          } else if (OB_FAIL(merge_file_cell_kind(merged[i], kind))) {
            LOG_WARN("failed to merge csv cell kind", K(i), K(ret));
          } else if (FCK_NULL == kind) {
            saw_null[i] = true;
          }
        }
        ++data_line_no_;
        ++sampled;
      }
    } else {
      if (OB_UNLIKELY(fields_.count() != col_count)) {
        ret = OB_INVALID_DATA;
        LOG_USER_ERROR(OB_INVALID_DATA);
        LOG_WARN("csv row field count mismatch during schema inference", K(path_),
            K(data_line_no_ + 1), K(fields_.count()), K(col_count), K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
          FileCellKind kind = FCK_NONE;
          if (OB_FAIL(classify_csv_cell_(field_quoted_.at(i), fields_.at(i), kind))) {
            LOG_WARN("failed to classify csv cell", K(i), K(ret));
          } else if (OB_FAIL(merge_file_cell_kind(merged[i], kind))) {
            LOG_WARN("failed to merge csv cell kind", K(i), K(ret));
          } else if (FCK_NULL == kind) {
            saw_null[i] = true;
          }
        }
        ++data_line_no_;
        ++sampled;
      }
    }
    if (OB_SUCC(ret) && !unlimited && sampled >= sample_limit) {
      break;
    }
  }
  if (OB_SUCC(ret)) {
    if (0 == col_count) {
      // empty file (or header only): produce one default column
      FileColumnMeta col;
      col.name_ = ObString("COLUMN_0");
      col.type_.set_varchar();
      col.type_.set_collation_level(CS_LEVEL_IMPLICIT);
      col.type_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      cell_kinds_.push_back(FCK_NONE);
      if (OB_FAIL(columns_.push_back(col))) {
        LOG_WARN("failed to push default column", K(ret));
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
        ObObjMeta type;
        if (OB_FAIL(file_cell_kind_to_type(merged[i], type))) {
          LOG_WARN("failed to map csv column type", K(i), K(merged[i]), K(ret));
        } else if (OB_FAIL(cell_kinds_.push_back(merged[i]))) {
          LOG_WARN("failed to push csv cell kind", K(ret));
        } else {
          columns_.at(i).type_ = type;
          // 2026-11-07: DESCRIBE null 列口径——采样中见过 NULL/未采样/全 NULL 列可空
          columns_.at(i).nullable_ = saw_null[i] || FCK_NULL == merged[i] || FCK_NONE == merged[i];
        }
      }
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
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("csv file table reader is not inited", K(ret));
  } else if (options_.max_rows_ > 0 && rows_emitted_ >= options_.max_rows_) {
    // max rows reached
  } else {
    bool has_row = false;
    while (OB_SUCC(ret) && !has_next) {
      if (OB_FAIL(read_csv_row_(has_row))) {
        LOG_WARN("failed to read csv data row", K(path_), K(ret));
      } else if (!has_row) {
        // eof or empty row
        char c = '\0';
        bool eof_now = false;
        if (OB_FAIL(peek_char_(c, eof_now))) {
          LOG_WARN("failed to peek csv stream", K(ret));
        } else if (eof_now) {
          // iteration end
          break;
        }
        // otherwise: an empty line, skip it and continue
      } else if (OB_UNLIKELY(fields_.count() != columns_.count())) {
        ret = OB_INVALID_DATA;
        LOG_USER_ERROR(OB_INVALID_DATA);
        LOG_WARN("csv row field count mismatch", K(path_), K(data_line_no_ + 1),
            K(fields_.count()), K(columns_.count()), K(ret));
      } else {
        ++data_line_no_;
        for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
          FileCellKind kind = FCK_NONE;
          ObObj obj;
          const ObString &text = fields_.at(i);
          if (OB_FAIL(classify_csv_cell_(field_quoted_.at(i), text, kind))) {
            LOG_WARN("failed to classify csv cell", K(i), K(ret));
          } else if (FCK_NULL == kind) {
            obj.set_null();
          } else if (OB_FAIL(file_cell_text_to_obj(text, columns_.at(i).type_, kind, obj))) {
            LOG_USER_ERROR(OB_INVALID_DATA);
            LOG_WARN("failed to convert csv cell to obj", K(path_), K(data_line_no_), K(i),
                K(text), K(ret));
          }
          if (OB_SUCC(ret) && OB_FAIL(row.push_back(obj))) {
            LOG_WARN("failed to push csv row cell", K(i), K(ret));
          }
        }
        if (OB_SUCC(ret)) {
          ++rows_emitted_;
          has_next = true;
        }
      }
    }
  }
  return ret;
}

int CsvFileTableReader::reuse()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(fp_) && inited_) {
    data_line_no_ = 0;
    rows_emitted_ = 0;
    clear_row_state_();
    if (OB_FAIL(rewind_to_data_())) {
      LOG_WARN("failed to rewind csv file for reuse", K(path_), K(ret));
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
      stat.inode_ = st.st_ino;
#if defined(__linux__)
      stat.modify_nsec_ = st.st_mtim.tv_nsec;
#elif defined(__APPLE__)
      stat.modify_nsec_ = st.st_mtimespec.tv_nsec;
#endif
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
  if (OB_NOT_NULL(read_buf_)) {
    alloc_.free(read_buf_);
    read_buf_ = NULL;
  }
  buf_len_ = 0;
  buf_pos_ = 0;
  if (OB_NOT_NULL(val_buf_)) {
    alloc_.free(val_buf_);
    val_buf_ = NULL;
  }
  val_buf_cap_ = 0;
  val_buf_used_ = 0;
  inited_ = false;
}

} // end namespace sql
} // end namespace oceanbase
