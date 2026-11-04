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

#include "sql/engine/file_scan/ob_json_file_table_reader.h"

#include <stdio.h>
#include <string.h>

#include "lib/charset/ob_charset.h"
#include "lib/ob_define.h"
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

namespace
{
bool is_json_ws(char c)
{
  return ' ' == c || '\t' == c || '\r' == c || '\n' == c;
}

int hex_digit_val(char c, int32_t &val)
{
  int ret = OB_SUCCESS;
  if (c >= '0' && c <= '9') {
    val = c - '0';
  } else if (c >= 'a' && c <= 'f') {
    val = c - 'a' + 10;
  } else if (c >= 'A' && c <= 'F') {
    val = c - 'A' + 10;
  } else {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

// encode a unicode code point as utf-8 into the buffer
int append_utf8_code_point(char *buf, int64_t cap, int64_t &used, uint32_t cp)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(used + 4 > cap)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("failed to append utf8 code point", K(used), K(cap), K(ret));
  } else if (cp <= 0x7F) {
    buf[used++] = static_cast<char>(cp);
  } else if (cp <= 0x7FF) {
    buf[used++] = static_cast<char>(0xC0 | (cp >> 6));
    buf[used++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp <= 0xFFFF) {
    buf[used++] = static_cast<char>(0xE0 | (cp >> 12));
    buf[used++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[used++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    buf[used++] = static_cast<char>(0xF0 | (cp >> 18));
    buf[used++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    buf[used++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[used++] = static_cast<char>(0x80 | (cp & 0x3F));
  }
  return ret;
}
} // end anonymous namespace

JsonFileTableReader::JsonFileTableReader(ObIAllocator &alloc, FileTableFormat format)
    : alloc_(alloc),
      format_(format),
      fp_(NULL),
      read_buf_(NULL),
      buf_len_(0),
      buf_pos_(0),
      line_buf_(NULL),
      line_cap_(0),
      line_used_(0),
      val_buf_(NULL),
      val_cap_(0),
      val_used_(0),
      line_no_(0),
      rows_emitted_(0),
      inited_(false)
{}

JsonFileTableReader::~JsonFileTableReader()
{
  close();
}

int JsonFileTableReader::dup_string_(const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.length() > INT32_MAX)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("string too long", K(src.length()), K(ret));
  } else if (src.empty()) {
    dst = src;
  } else {
    char *buf = static_cast<char *>(alloc_.alloc(src.length()));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to alloc memory for string", K(src.length()), K(ret));
    } else {
      MEMCPY(buf, src.ptr(), static_cast<size_t>(src.length()));
      dst.assign_ptr(buf, static_cast<int32_t>(src.length()));
    }
  }
  return ret;
}

int JsonFileTableReader::grow_buf_(char *&buf, int64_t &cap, int64_t need, int64_t used_len)
{
  int ret = OB_SUCCESS;
  if (need <= cap) {
    // fast path
  } else {
    int64_t new_cap = (cap <= 0 ? INIT_LINE_BUF_SIZE : cap);
    while (new_cap < need) {
      new_cap *= 2;
    }
    if (OB_UNLIKELY(new_cap > MAX_LINE_BUF_SIZE)) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("file table line buffer too large", K(new_cap), K(need), K(ret));
    } else {
      char *new_buf = static_cast<char *>(alloc_.alloc(new_cap));
      if (OB_ISNULL(new_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to alloc line buffer", K(new_cap), K(ret));
      } else {
        if (OB_NOT_NULL(buf) && used_len > 0) {
          MEMCPY(new_buf, buf, static_cast<size_t>(used_len));
        }
        if (OB_NOT_NULL(buf) && cap > 0) {
          alloc_.free(buf);
        }
        buf = new_buf;
        cap = new_cap;
      }
    }
  }
  return ret;
}

int JsonFileTableReader::next_char_(char &c, bool &eof)
{
  int ret = OB_SUCCESS;
  eof = false;
  if (buf_pos_ >= buf_len_) {
    buf_len_ = ::fread(read_buf_, 1, static_cast<size_t>(READ_BUF_SIZE), fp_);
    buf_pos_ = 0;
    if (0 == buf_len_) {
      if (0 != ::ferror(fp_)) {
        ret = OB_IO_ERROR;
        LOG_WARN("failed to read json file", K(path_), K(ret));
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

int JsonFileTableReader::peek_char_(char &c, bool &eof)
{
  int ret = OB_SUCCESS;
  eof = false;
  if (buf_pos_ >= buf_len_) {
    buf_len_ = ::fread(read_buf_, 1, static_cast<size_t>(READ_BUF_SIZE), fp_);
    buf_pos_ = 0;
    if (0 == buf_len_) {
      if (0 != ::ferror(fp_)) {
        ret = OB_IO_ERROR;
        LOG_WARN("failed to read json file", K(path_), K(ret));
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

int JsonFileTableReader::read_line_(bool &has_line)
{
  int ret = OB_SUCCESS;
  has_line = false;
  line_used_ = 0;
  while (OB_SUCC(ret)) {
    char c = '\0';
    bool eof = false;
    if (OB_FAIL(next_char_(c, eof))) {
      LOG_WARN("failed to read json stream", K(path_), K(ret));
    } else if (eof) {
      if (line_used_ > 0) {
        has_line = true;
      }
      break;
    } else if ('\n' == c) {
      has_line = true;
      break;
    } else {
      if (OB_FAIL(grow_buf_(line_buf_, line_cap_, line_used_ + 1, line_used_))) {
        LOG_WARN("failed to grow json line buffer", K(line_used_), K(ret));
      } else {
        line_buf_[line_used_++] = c;
      }
    }
  }
  if (OB_SUCC(ret) && has_line && line_used_ > 0 && '\r' == line_buf_[line_used_ - 1]) {
    --line_used_;
  }
  return ret;
}

void JsonFileTableReader::clear_line_state_()
{
  val_used_ = 0;
}

int JsonFileTableReader::find_or_add_column_(const ObString &name, int64_t &col_idx)
{
  int ret = OB_SUCCESS;
  col_idx = -1;
  for (int64_t i = 0; col_idx < 0 && i < columns_.count(); ++i) {
    if (columns_.at(i).name_.length() == name.length()
        && 0 == MEMCMP(columns_.at(i).name_.ptr(), name.ptr(),
                   static_cast<size_t>(name.length()))) {
      col_idx = i;
    }
  }
  if (col_idx < 0) {
    if (columns_.count() >= OB_FILE_TABLE_MAX_COLUMN_COUNT) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("too many columns in json file", K(columns_.count()), K(ret));
    } else {
      FileColumnMeta col;
      if (OB_FAIL(dup_string_(name, col.name_))) {
        LOG_WARN("failed to copy json column name", K(ret));
      } else if (OB_FAIL(columns_.push_back(col))) {
        LOG_WARN("failed to push json column meta", K(ret));
      } else if (OB_FAIL(cell_kinds_.push_back(FCK_NONE))) {
        LOG_WARN("failed to push json cell kind", K(ret));
      } else {
        col_idx = columns_.count() - 1;
      }
    }
  }
  return ret;
}

int JsonFileTableReader::parse_json_string_(int64_t &pos, ObString &text)
{
  int ret = OB_SUCCESS;
  // pos points at the opening quote; the decoded value is appended to val_buf_
  // (enough space is reserved by the caller so the buffer never grows)
  ++pos;  // skip the opening quote
  const int64_t start = val_used_;
  bool closed = false;
  while (OB_SUCC(ret) && !closed) {
    if (pos >= line_used_) {
      ret = OB_INVALID_DATA;
      LOG_WARN("unterminated json string", K(path_), K(line_no_), K(ret));
    } else {
      const char c = line_buf_[pos];
      if ('"' == c) {
        ++pos;
        closed = true;
      } else if ('\\' == c) {
        ++pos;
        if (pos >= line_used_) {
          ret = OB_INVALID_DATA;
          LOG_WARN("unterminated json escape", K(path_), K(line_no_), K(ret));
        } else {
          const char e = line_buf_[pos++];
          switch (e) {
            case '"':
            case '\\':
            case '/':
              val_buf_[val_used_++] = e;
              break;
            case 'b':
              val_buf_[val_used_++] = '\b';
              break;
            case 'f':
              val_buf_[val_used_++] = '\f';
              break;
            case 'n':
              val_buf_[val_used_++] = '\n';
              break;
            case 'r':
              val_buf_[val_used_++] = '\r';
              break;
            case 't':
              val_buf_[val_used_++] = '\t';
              break;
            case 'u': {
              int64_t cp = 0;
              int32_t hex_val = 0;
              for (int64_t i = 0; OB_SUCC(ret) && i < 4; ++i) {
                if (pos >= line_used_) {
                  ret = OB_INVALID_DATA;
                  LOG_WARN("truncated \\u escape in json string", K(path_), K(line_no_), K(ret));
                } else if (OB_FAIL(hex_digit_val(line_buf_[pos++], hex_val))) {
                  LOG_WARN("invalid \\u escape in json string", K(path_), K(line_no_), K(ret));
                } else {
                  cp = cp * 16 + hex_val;
                }
              }
              if (OB_SUCC(ret) && cp >= 0xD800 && cp <= 0xDBFF) {
                // high surrogate: expect a "\uXXXX" low surrogate next
                if (pos + 2 > line_used_ || '\\' != line_buf_[pos] || 'u' != line_buf_[pos + 1]) {
                  ret = OB_INVALID_DATA;
                  LOG_WARN("missing low surrogate in json string", K(path_), K(line_no_), K(ret));
                } else {
                  pos += 2;
                  int64_t low = 0;
                  for (int64_t i = 0; OB_SUCC(ret) && i < 4; ++i) {
                    if (pos >= line_used_) {
                      ret = OB_INVALID_DATA;
                      LOG_WARN("truncated \\u escape in json string", K(path_), K(line_no_),
                          K(ret));
                    } else if (OB_FAIL(hex_digit_val(line_buf_[pos++], hex_val))) {
                      LOG_WARN("invalid \\u escape in json string", K(path_), K(line_no_), K(ret));
                    } else {
                      low = low * 16 + hex_val;
                    }
                  }
                  if (OB_SUCC(ret)) {
                    if (low < 0xDC00 || low > 0xDFFF) {
                      ret = OB_INVALID_DATA;
                      LOG_WARN("invalid low surrogate in json string", K(path_), K(line_no_),
                          K(ret));
                    } else {
                      cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                  }
                }
              }
              if (OB_SUCC(ret)) {
                if (OB_FAIL(append_utf8_code_point(val_buf_, val_cap_, val_used_,
                        static_cast<uint32_t>(cp)))) {
                  LOG_WARN("failed to append utf8 code point", K(cp), K(ret));
                }
              }
              break;
            }
            default:
              ret = OB_INVALID_DATA;
              LOG_WARN("invalid json escape char", K(path_), K(line_no_), K(e), K(ret));
              break;
          }
        }
      } else {
        val_buf_[val_used_++] = c;
        ++pos;
      }
    }
  }
  if (OB_SUCC(ret)) {
    text.assign_ptr(val_buf_ + start, static_cast<int32_t>(val_used_ - start));
  }
  return ret;
}

int JsonFileTableReader::handle_kv_(bool for_infer, int64_t line_no, const ObString &name,
    const ObString &text, FileCellKind kind)
{
  int ret = OB_SUCCESS;
  int64_t col_idx = -1;
  if (OB_FAIL(find_or_add_column_(name, col_idx))) {
    LOG_WARN("failed to find or add json column", K(name), K(ret));
  } else if (col_idx < 0) {
    // a field that is not in the frozen schema (the file was appended after
    // init): skip it
  } else if (for_infer) {
    FileCellKind merged = cell_kinds_.at(col_idx);
    const FileCellKind prev = merged;
    if (OB_FAIL(merge_file_cell_kind(merged, kind))) {
      LOG_WARN("failed to merge json cell kind", K(name), K(kind), K(ret));
    } else if (FCK_STRING == merged && FCK_STRING != prev && FCK_STRING != kind) {
      ret = OB_INVALID_DATA;
      LOG_USER_ERROR(OB_INVALID_DATA);
      LOG_WARN("json column type conflict that cannot be widened", K(path_), K(line_no), K(name),
          K(prev), K(kind), K(ret));
    } else {
      cell_kinds_.at(col_idx) = merged;
    }
  } else {
    ObObj obj;
    if (FCK_NULL == kind) {
      obj.set_null();
    } else if (OB_FAIL(file_cell_text_to_obj(text, columns_.at(col_idx).type_, kind, obj))) {
      LOG_USER_ERROR(OB_INVALID_DATA);
      LOG_WARN("failed to convert json value to obj", K(path_), K(line_no), K(name), K(text),
          K(ret));
    }
    if (OB_SUCC(ret)) {
      if (col_idx >= row_set_flags_.count() || col_idx >= row_objs_.count()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("json row cell buffer is too small", K(col_idx), K(ret));
      } else {
        row_set_flags_.at(col_idx) = true;
        row_objs_.at(col_idx) = obj;
      }
    }
  }
  return ret;
}

int JsonFileTableReader::parse_json_line_(bool for_infer, bool &is_blank)
{
  int ret = OB_SUCCESS;
  is_blank = false;
  int64_t pos = 0;
  while (pos < line_used_ && is_json_ws(line_buf_[pos])) {
    ++pos;
  }
  if (pos >= line_used_) {
    is_blank = true;
  } else if ('{' != line_buf_[pos]) {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_DATA);
    LOG_WARN("json line is not a flat object", K(path_), K(line_no_), K(format_), K(ret));
  } else if (OB_FAIL(
                 grow_buf_(val_buf_, val_cap_, 4 * line_used_ + 16, val_used_))) {
    // reserve the worst-case space for the decoded strings of the whole line in
    // one shot so that value pointers into val_buf_ stay stable
    LOG_WARN("failed to reserve json line value buffer", K(line_used_), K(ret));
  } else {
    clear_line_state_();
    ++pos;
    // parse key/value pairs
    bool obj_closed = false;
    while (OB_SUCC(ret) && !obj_closed) {
      while (pos < line_used_ && is_json_ws(line_buf_[pos])) {
        ++pos;
      }
      if (pos >= line_used_) {
        ret = OB_INVALID_DATA;
        LOG_WARN("unterminated json object", K(path_), K(line_no_), K(ret));
      } else if ('}' == line_buf_[pos]) {
        ++pos;
        obj_closed = true;
      } else {
        // parse the key
        ObString key;
        if ('"' != line_buf_[pos]) {
          ret = OB_INVALID_DATA;
          LOG_WARN("json object key must be a string", K(path_), K(line_no_), K(ret));
        } else if (OB_FAIL(parse_json_string_(pos, key))) {
          LOG_WARN("failed to parse json object key", K(path_), K(line_no_), K(ret));
        } else {
          while (pos < line_used_ && is_json_ws(line_buf_[pos])) {
            ++pos;
          }
          if (pos >= line_used_ || ':' != line_buf_[pos]) {
            ret = OB_INVALID_DATA;
            LOG_WARN("missing ':' after json object key", K(path_), K(line_no_), K(ret));
          } else {
            ++pos;
            while (pos < line_used_ && is_json_ws(line_buf_[pos])) {
              ++pos;
            }
            if (pos >= line_used_) {
              ret = OB_INVALID_DATA;
              LOG_WARN("missing json value", K(path_), K(line_no_), K(ret));
            } else {
              const char c = line_buf_[pos];
              ObString text;
              FileCellKind kind = FCK_NULL;
              if ('"' == c) {
                if (OB_FAIL(parse_json_string_(pos, text))) {
                  LOG_WARN("failed to parse json string value", K(path_), K(line_no_), K(ret));
                } else if (text.empty()) {
                  // an explicit json empty string is a string, not null
                  kind = FCK_STRING;
                } else if (OB_FAIL(classify_file_cell_text(text, ObString(), kind))) {
                  LOG_WARN("failed to classify json string value", K(text), K(ret));
                }
              } else if ('t' == c || 'f' == c) {
                const int64_t lit_len = ('t' == c ? 4 : 5);
                const char *lit = ('t' == c ? "true" : "false");
                if (pos + lit_len > line_used_
                    || 0 != MEMCMP(line_buf_ + pos, lit, static_cast<size_t>(lit_len))) {
                  ret = OB_INVALID_DATA;
                  LOG_WARN("invalid json bool literal", K(path_), K(line_no_), K(ret));
                } else {
                  text.assign_ptr(line_buf_ + pos, static_cast<int32_t>(lit_len));
                  pos += lit_len;
                  kind = FCK_BOOL;
                }
              } else if ('n' == c) {
                if (pos + 4 > line_used_ || 0 != MEMCMP(line_buf_ + pos, "null", 4)) {
                  ret = OB_INVALID_DATA;
                  LOG_WARN("invalid json null literal", K(path_), K(line_no_), K(ret));
                } else {
                  pos += 4;
                  kind = FCK_NULL;
                }
              } else if ('{' == c || '[' == c) {
                ret = OB_INVALID_DATA;
                LOG_USER_ERROR(OB_INVALID_DATA);
                LOG_WARN("nested json object/array value is not supported", K(path_), K(line_no_),
                    K(key), K(ret));
              } else {
                // number token
                const int64_t num_start = pos;
                while (pos < line_used_
                       && ((line_buf_[pos] >= '0' && line_buf_[pos] <= '9')
                           || '+' == line_buf_[pos] || '-' == line_buf_[pos]
                           || '.' == line_buf_[pos] || 'e' == line_buf_[pos]
                           || 'E' == line_buf_[pos])) {
                  ++pos;
                }
                text.assign_ptr(line_buf_ + num_start, static_cast<int32_t>(pos - num_start));
                int64_t ival = 0;
                double dval = 0.0;
                if (file_parse_int64_text(text.ptr(), text.length(), ival)) {
                  kind = FCK_INT;
                } else if (file_parse_double_text(text.ptr(), text.length(), dval)) {
                  kind = FCK_DOUBLE;
                } else {
                  ret = OB_INVALID_DATA;
                  LOG_WARN("invalid json number", K(path_), K(line_no_), K(text), K(ret));
                }
              }
              if (OB_SUCC(ret)) {
                if (OB_FAIL(handle_kv_(for_infer, line_no_, key, text, kind))) {
                  LOG_WARN("failed to handle json key/value pair", K(path_), K(line_no_), K(key),
                      K(ret));
                } else {
                  // continue with ',' or '}'
                  while (pos < line_used_ && is_json_ws(line_buf_[pos])) {
                    ++pos;
                  }
                  if (pos >= line_used_) {
                    ret = OB_INVALID_DATA;
                    LOG_WARN("unterminated json object", K(path_), K(line_no_), K(ret));
                  } else if (',' == line_buf_[pos]) {
                    ++pos;
                  } else if ('}' == line_buf_[pos]) {
                    ++pos;
                    obj_closed = true;
                  } else {
                    ret = OB_INVALID_DATA;
                    LOG_WARN("unexpected char in json object", K(path_), K(line_no_), K(ret));
                  }
                }
              }
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      // only whitespace is allowed after the closing brace
      while (pos < line_used_ && is_json_ws(line_buf_[pos])) {
        ++pos;
      }
      if (pos < line_used_) {
        ret = OB_INVALID_DATA;
        LOG_USER_ERROR(OB_INVALID_DATA);
        LOG_WARN("trailing content after json object", K(path_), K(line_no_), K(ret));
      }
    }
  }
  return ret;
}

int JsonFileTableReader::infer_schema_()
{
  int ret = OB_SUCCESS;
  buf_len_ = 0;
  buf_pos_ = 0;
  line_no_ = 0;
  ::rewind(fp_);
  while (OB_SUCC(ret)) {
    bool has_line = false;
    if (OB_FAIL(read_line_(has_line))) {
      LOG_WARN("failed to read json line", K(path_), K(ret));
    } else if (!has_line) {
      break;
    } else {
      ++line_no_;
      bool is_blank = false;
      if (OB_FAIL(parse_json_line_(true, is_blank))) {
        LOG_WARN("failed to parse json line during schema inference", K(path_), K(line_no_), K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (0 == columns_.count()) {
      // empty file: produce one default column
      FileColumnMeta col;
      col.name_ = ObString("COLUMN_0");
      col.type_.set_varchar();
      col.type_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      if (OB_FAIL(columns_.push_back(col))) {
        LOG_WARN("failed to push default json column", K(ret));
      } else if (OB_FAIL(cell_kinds_.push_back(FCK_NONE))) {
        LOG_WARN("failed to push default json cell kind", K(ret));
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
      ObObjMeta type;
      if (OB_FAIL(file_cell_kind_to_type(cell_kinds_.at(i), type))) {
        LOG_WARN("failed to map json column type", K(i), K(cell_kinds_.at(i)), K(ret));
      } else {
        columns_.at(i).type_ = type;
      }
    }
  }
  if (OB_SUCC(ret)) {
    // rewind for the data scan phase
    buf_len_ = 0;
    buf_pos_ = 0;
    line_no_ = 0;
    clear_line_state_();
    ::rewind(fp_);
  }
  return ret;
}

int JsonFileTableReader::init(const ObString &path, const FileReadOptions &options)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(path.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("file table path is empty", K(ret));
  } else if (OB_FAIL(dup_string_(path, path_))) {
    LOG_WARN("failed to copy json path", K(ret));
  } else {
    options_.parse_header_ = options.parse_header_;
    options_.sample_size_ = options.sample_size_;
    options_.max_rows_ = options.max_rows_;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(read_buf_ = static_cast<char *>(alloc_.alloc(READ_BUF_SIZE)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to alloc json read buffer", K(ret));
  } else if (OB_ISNULL(fp_ = ::fopen(path_.ptr(), "rb"))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_USER_ERROR(OB_FILE_NOT_EXIST, path_.length(), path_.ptr());
    LOG_WARN("failed to open json file", K(path_), K(ret));
  } else if (OB_FAIL(get_file_stat(file_stat_))) {
    LOG_WARN("failed to get file stat", K(path_), K(ret));
  } else if (OB_UNLIKELY(file_stat_.file_size_ > OB_FILE_TABLE_MAX_FILE_SIZE)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("json file is too large", K(path_), K(file_stat_.file_size_), K(ret));
  } else {
    inited_ = true;
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(infer_schema_())) {
      LOG_WARN("failed to infer json schema", K(path_), K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    // freeze the column schema for the data scan phase
    row_set_flags_.reserve(columns_.count());
    row_objs_.reserve(columns_.count());
    for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
      if (OB_FAIL(row_set_flags_.push_back(false))) {
        LOG_WARN("failed to init row set flags", K(ret));
      } else {
        ObObj null_obj;
        null_obj.set_null();
        if (OB_FAIL(row_objs_.push_back(null_obj))) {
          LOG_WARN("failed to init row objs", K(ret));
        }
      }
    }
  }
  if (OB_FAIL(ret)) {
    close();
  }
  return ret;
}

int JsonFileTableReader::get_column_metas(ObIArray<FileColumnMeta> &columns) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("json file table reader is not inited", K(ret));
  } else if (OB_FAIL(columns.assign(columns_))) {
    LOG_WARN("failed to assign column metas", K(ret));
  }
  return ret;
}

int JsonFileTableReader::get_next_row(ObIArray<ObObj> &row, bool &has_next)
{
  int ret = OB_SUCCESS;
  has_next = false;
  row.reuse();
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("json file table reader is not inited", K(ret));
  } else if (options_.max_rows_ > 0 && rows_emitted_ >= options_.max_rows_) {
    // max rows reached
  } else {
    bool has_line = false;
    bool row_done = false;
    while (OB_SUCC(ret) && !row_done) {
      if (OB_FAIL(read_line_(has_line))) {
        LOG_WARN("failed to read json line", K(path_), K(ret));
      } else if (!has_line) {
        // eof
        row_done = true;
      } else {
        ++line_no_;
        bool is_blank = false;
        if (OB_FAIL(parse_json_line_(false, is_blank))) {
          LOG_WARN("failed to parse json line", K(path_), K(line_no_), K(ret));
        } else if (is_blank) {
          // skip blank lines
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
            if (!row_set_flags_.at(i)) {
              ObObj null_obj;
              null_obj.set_null();
              if (OB_FAIL(row.push_back(null_obj))) {
                LOG_WARN("failed to push missing json cell", K(i), K(ret));
              }
            } else if (OB_FAIL(row.push_back(row_objs_.at(i)))) {
              LOG_WARN("failed to push json cell", K(i), K(ret));
            }
            if (OB_SUCC(ret)) {
              row_set_flags_.at(i) = false;
            }
          }
          if (OB_SUCC(ret)) {
            ++rows_emitted_;
            has_next = true;
            row_done = true;
          }
        }
      }
    }
  }
  return ret;
}

int JsonFileTableReader::reuse()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(fp_) && inited_) {
    line_no_ = 0;
    rows_emitted_ = 0;
    buf_len_ = 0;
    buf_pos_ = 0;
    clear_line_state_();
    for (int64_t i = 0; OB_SUCC(ret) && i < row_set_flags_.count(); ++i) {
      row_set_flags_.at(i) = false;
    }
    ::rewind(fp_);
  }
  return ret;
}

int JsonFileTableReader::get_file_stat(FileTableFileStat &stat) const
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
      LOG_WARN("failed to stat json file", K(path_), K(ret));
    } else {
      stat.file_size_ = static_cast<int64_t>(st.st_size);
      stat.last_modify_time_ = static_cast<int64_t>(st.st_mtime);
    }
  }
  return ret;
}

void JsonFileTableReader::close()
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
  if (OB_NOT_NULL(line_buf_)) {
    alloc_.free(line_buf_);
    line_buf_ = NULL;
  }
  line_cap_ = 0;
  line_used_ = 0;
  if (OB_NOT_NULL(val_buf_)) {
    alloc_.free(val_buf_);
    val_buf_ = NULL;
  }
  val_cap_ = 0;
  val_used_ = 0;
  inited_ = false;
}

} // end namespace sql
} // end namespace oceanbase
