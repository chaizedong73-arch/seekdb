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

#include "sql/engine/file_scan/ob_file_table_type_infer.h"

#include <stdlib.h>
#include <string.h>

#include "lib/charset/ob_charset.h"
#include "lib/ob_define.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{
// The max text length of a numeric literal we are willing to classify as
// BIGINT/DOUBLE. Longer numerics fall back to VARCHAR deterministically.
constexpr int64_t MAX_NUMERIC_TEXT_LEN = 64;
constexpr int64_t MAX_DATETIME_FRACTION_DIGITS = 6;
constexpr int64_t USEC_PER_SEC_LOCAL = 1000000LL;
constexpr int64_t SECS_PER_DAY = 86400LL;

bool is_digit(char c)
{
  return c >= '0' && c <= '9';
}

int32_t digit_val(char c)
{
  return c - '0';
}

bool is_leap_year(int32_t year)
{
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

bool is_valid_date(int32_t year, int32_t mon, int32_t mday)
{
  bool bret = false;
  if (year >= 1 && year <= 9999 && mon >= 1 && mon <= 12) {
    static const int32_t MON_DAYS[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int32_t max_day = MON_DAYS[mon];
    if (2 == mon && is_leap_year(year)) {
      max_day = 29;
    }
    bret = mday >= 1 && mday <= max_day;
  }
  return bret;
}

// Strict int64 text: optional +/- followed by at least one digit, the whole
// text is consumed. Values out of int64 range return false (caller falls back
// to double classification).
bool parse_int64_text(const char *s, const int64_t len, int64_t &val)
{
  bool bret = false;
  if (len > 0 && len <= MAX_NUMERIC_TEXT_LEN) {
    int64_t pos = 0;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') {
      neg = (s[0] == '-');
      pos = 1;
    }
    if (pos < len) {
      const uint64_t LIMIT = neg
          ? (static_cast<uint64_t>(INT64_MAX) + 1)   // INT64_MIN magnitude
          : static_cast<uint64_t>(INT64_MAX);
      uint64_t acc = 0;
      bool overflow = false;
      for (; pos < len; ++pos) {
        const char c = s[pos];
        if (!is_digit(c)) {
          break;
        }
        const uint64_t digit = static_cast<uint64_t>(digit_val(c));
        if (acc > (LIMIT - digit) / 10) {
          overflow = true;
          break;
        }
        acc = acc * 10 + digit;
      }
      if (!overflow && pos == len) {
        if (neg) {
          val = (acc == static_cast<uint64_t>(INT64_MAX) + 1)
              ? INT64_MIN
              : -static_cast<int64_t>(acc);
        } else {
          val = static_cast<int64_t>(acc);
        }
        bret = true;
      }
    }
  }
  return bret;
}

// Strict double text: [+-]? (digits [ . digits?] | . digits) ([eE][+-]?digits)?
// The whole text must be consumed. Unlike strtod(), "inf"/"nan"/"0x..." are
// rejected by the structural check.
bool parse_double_text(const char *s, const int64_t len, double &val)
{
  bool bret = false;
  if (len > 0 && len <= MAX_NUMERIC_TEXT_LEN) {
    int64_t pos = 0;
    if (s[0] == '+' || s[0] == '-') {
      ++pos;
    }
    int64_t int_digits = 0;
    int64_t frac_digits = 0;
    while (pos < len && is_digit(s[pos])) {
      ++pos;
      ++int_digits;
    }
    if (pos < len && s[pos] == '.') {
      ++pos;
      while (pos < len && is_digit(s[pos])) {
        ++pos;
        ++frac_digits;
      }
    }
    if (int_digits + frac_digits > 0 && pos == len) {
      // plain integer/decimal text
      bret = true;
    } else if (int_digits + frac_digits > 0 && pos < len && (s[pos] == 'e' || s[pos] == 'E')) {
      ++pos;
      if (pos < len && (s[pos] == '+' || s[pos] == '-')) {
        ++pos;
      }
      int64_t exp_digits = 0;
      while (pos < len && is_digit(s[pos])) {
        ++pos;
        ++exp_digits;
      }
      bret = exp_digits > 0 && pos == len;
    }
    if (bret) {
      char buf[MAX_NUMERIC_TEXT_LEN + 1];
      MEMCPY(buf, s, static_cast<size_t>(len));
      buf[len] = '\0';
      char *end = NULL;
      val = ::strtod(buf, &end);
      bret = (end == buf + len);
    }
  }
  return bret;
}

bool parse_bool_text(const char *s, const int64_t len, bool &val)
{
  bool bret = false;
  if (4 == len && 0 == strncasecmp(s, "true", 4)) {
    val = true;
    bret = true;
  } else if (5 == len && 0 == strncasecmp(s, "false", 5)) {
    val = false;
    bret = true;
  }
  return bret;
}

// Strict date text: YYYY-MM-DD, the whole text must be consumed and the civil
// date must be valid (month/day range and leap years).
bool parse_date_text(const char *s, const int64_t len, int32_t &year, int32_t &mon, int32_t &mday)
{
  bool bret = false;
  if (10 == len && '-' == s[4] && '-' == s[7]) {
    bool all_digit = true;
    for (int64_t i = 0; all_digit && i < len; ++i) {
      if (4 != i && 7 != i && !is_digit(s[i])) {
        all_digit = false;
      }
    }
    if (all_digit) {
      year = digit_val(s[0]) * 1000 + digit_val(s[1]) * 100 + digit_val(s[2]) * 10
          + digit_val(s[3]);
      mon = digit_val(s[5]) * 10 + digit_val(s[6]);
      mday = digit_val(s[8]) * 10 + digit_val(s[9]);
      bret = is_valid_date(year, mon, mday);
    }
  }
  return bret;
}

// Strict datetime text: YYYY-MM-DD[ T]HH:MM[:SS[.fraction]]
bool parse_datetime_text(const char *s, const int64_t len, int32_t &year, int32_t &mon,
    int32_t &mday, int32_t &hour, int32_t &minute, int32_t &sec, int32_t &usec)
{
  bool bret = false;
  usec = 0;
  if (len >= 16 && len <= 10 + 1 + 5 + 1 + 2 + 1 + MAX_DATETIME_FRACTION_DIGITS) {
    if (' ' == s[10] || 'T' == s[10]) {
      if (':' == s[13] && is_digit(s[11]) && is_digit(s[12]) && is_digit(s[14])
          && is_digit(s[15])) {
        hour = digit_val(s[11]) * 10 + digit_val(s[12]);
        minute = digit_val(s[14]) * 10 + digit_val(s[15]);
        int64_t pos = 16;
        if (pos < len && ':' == s[pos]) {
          ++pos;
          if (pos + 2 <= len && is_digit(s[pos]) && is_digit(s[pos + 1])) {
            sec = digit_val(s[pos]) * 10 + digit_val(s[pos + 1]);
            pos += 2;
            if (pos < len && '.' == s[pos]) {
              ++pos;
              int64_t frac_digits = 0;
              int32_t frac_val = 0;
              while (pos < len && is_digit(s[pos])) {
                if (frac_digits < MAX_DATETIME_FRACTION_DIGITS) {
                  frac_val = frac_val * 10 + digit_val(s[pos]);
                }
                ++frac_digits;
                ++pos;
              }
              if (frac_digits < 1 || frac_digits > MAX_DATETIME_FRACTION_DIGITS) {
                return false;
              }
              // right align the fraction digits to microseconds
              for (int64_t i = frac_digits; i < MAX_DATETIME_FRACTION_DIGITS; ++i) {
                frac_val *= 10;
              }
              usec = frac_val;
            }
          } else {
            return false;
          }
        } else {
          sec = 0;
        }
        if (pos == len && parse_date_text(s, 10, year, mon, mday) && hour >= 0 && hour <= 23
            && minute >= 0 && minute <= 59 && sec >= 0 && sec <= 59) {
          bret = true;
        }
      }
    }
  }
  return bret;
}

// days_from_civil: the number of days since 1970-01-01 (Howard Hinnant's
// algorithm), valid for the proleptic Gregorian calendar.
int64_t days_from_civil(int64_t y, const int64_t m, const int64_t d)
{
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;                          // [0, 399]
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  // [0, 146096]
  return era * 146097 + doe - 719468;
}

} // end anonymous namespace

bool file_parse_int64_text(const char *s, const int64_t len, int64_t &val)
{
  return parse_int64_text(s, len, val);
}

bool file_parse_double_text(const char *s, const int64_t len, double &val)
{
  return parse_double_text(s, len, val);
}

int classify_file_cell_text(const ObString &text, const ObString &null_str, FileCellKind &kind)
{
  int ret = OB_SUCCESS;
  const char *s = text.ptr();
  const int64_t len = text.length();
  kind = FCK_STRING;
  int64_t ival = 0;
  double dval = 0.0;
  bool bval = false;
  int32_t year = 0;
  int32_t mon = 0;
  int32_t mday = 0;
  int32_t hour = 0;
  int32_t minute = 0;
  int32_t sec = 0;
  int32_t usec = 0;
  // null matching: an empty null_str_ also matches empty text
  if (text.empty() && null_str.empty()) {
    kind = FCK_NULL;
  } else if (!null_str.empty() && null_str.length() == text.length()
             && 0 == MEMCMP(s, null_str.ptr(), static_cast<size_t>(len))) {
    kind = FCK_NULL;
  } else if (parse_int64_text(s, len, ival)) {
    kind = FCK_INT;
  } else if (parse_double_text(s, len, dval)) {
    kind = FCK_DOUBLE;
  } else if (parse_bool_text(s, len, bval)) {
    kind = FCK_BOOL;
  } else if (parse_date_text(s, len, year, mon, mday)) {
    kind = FCK_DATE;
  } else if (parse_datetime_text(s, len, year, mon, mday, hour, minute, sec, usec)) {
    kind = FCK_DATETIME;
  } else {
    kind = FCK_STRING;
  }
  return ret;
}

int merge_file_cell_kind(FileCellKind &acc, FileCellKind incoming)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(acc < FCK_NONE || acc >= FCK_MAX || incoming < FCK_NONE || incoming >= FCK_MAX)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid file cell kind", K(acc), K(incoming), K(ret));
  } else if (FCK_NONE == acc || FCK_NULL == acc) {
    acc = incoming;
  } else if (FCK_NONE == incoming || FCK_NULL == incoming) {
    // keep acc
  } else if (acc == incoming) {
    // keep acc
  } else if ((FCK_INT == acc && FCK_DOUBLE == incoming)
             || (FCK_DOUBLE == acc && FCK_INT == incoming)) {
    acc = FCK_DOUBLE;
  } else {
    acc = FCK_STRING;
  }
  return ret;
}

int file_cell_kind_to_type(FileCellKind kind, ObObjMeta &type)
{
  int ret = OB_SUCCESS;
  switch (kind) {
    case FCK_NONE:
    case FCK_NULL:
      type.set_null();
      break;
    case FCK_INT:
      type.set_int();
      break;
    case FCK_DOUBLE:
      type.set_double();
      break;
    case FCK_BOOL:
      type.set_tinyint();
      break;
    case FCK_DATE:
      type.set_date();
      break;
    case FCK_DATETIME:
      type.set_datetime();
      break;
    case FCK_STRING:
      type.set_varchar();
      type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      break;
    default:
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid file cell kind", K(kind), K(ret));
      break;
  }
  return ret;
}

int file_varchar_to_obj(const ObString &v, ObObj &obj)
{
  int ret = OB_SUCCESS;
  obj.set_varchar(v);
  obj.meta_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  return ret;
}

int file_int64_to_obj(int64_t v, ObObj &obj)
{
  int ret = OB_SUCCESS;
  obj.set_int(v);
  return ret;
}

int file_double_to_obj(double v, ObObj &obj)
{
  int ret = OB_SUCCESS;
  obj.set_double(v);
  return ret;
}

int file_bool_to_obj(bool v, ObObj &obj)
{
  int ret = OB_SUCCESS;
  obj.set_bool(v);
  return ret;
}

int file_date_to_obj(int32_t ob_date, ObObj &obj)
{
  int ret = OB_SUCCESS;
  obj.set_date(ob_date);
  return ret;
}

int file_datetime_to_obj(int64_t ob_datetime, ObObj &obj)
{
  int ret = OB_SUCCESS;
  obj.set_datetime(ob_datetime);
  return ret;
}

int file_civil_from_days(int64_t days_since_epoch, int32_t &year, int32_t &mon, int32_t &mday)
{
  int ret = OB_SUCCESS;
  int64_t z = days_since_epoch + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;  // [0, 146096]
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const int64_t yr = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  // [0, 365]
  const int64_t mp = (5 * doy + 2) / 153;                       // [0, 11]
  mday = static_cast<int32_t>(doy - (153 * mp + 2) / 5 + 1);    // [1, 31]
  mon = static_cast<int32_t>(mp + (mp < 10 ? 3 : -9));          // [1, 12]
  year = static_cast<int32_t>(yr + (mon <= 2));
  return ret;
}

int file_build_date(int32_t year, int32_t mon, int32_t mday, int32_t &ob_date)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_valid_date(year, mon, mday))) {
    ret = OB_INVALID_DATA;
    LOG_WARN("invalid civil date", K(year), K(mon), K(mday), K(ret));
  } else {
    ob_date = static_cast<int32_t>(days_from_civil(year, mon, mday));
  }
  return ret;
}

int file_build_datetime(int32_t year, int32_t mon, int32_t mday, int32_t hour, int32_t min,
    int32_t sec, int32_t usec, int64_t &ob_datetime)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_valid_date(year, mon, mday) || hour < 0 || hour > 23 || min < 0 || min > 59
                  || sec < 0 || sec > 59 || usec < 0 || usec >= USEC_PER_SEC_LOCAL)) {
    ret = OB_INVALID_DATA;
    LOG_WARN("invalid civil datetime", K(year), K(mon), K(mday), K(hour), K(min), K(sec),
        K(usec), K(ret));
  } else {
    const int64_t days = days_from_civil(year, mon, mday);
    ob_datetime = days * SECS_PER_DAY * USEC_PER_SEC_LOCAL
        + (static_cast<int64_t>(hour) * 3600 + static_cast<int64_t>(min) * 60 + sec)
            * USEC_PER_SEC_LOCAL
        + usec;
  }
  return ret;
}

int file_cell_text_to_obj(const ObString &text, const ObObjMeta &col_type,
    const FileCellKind cell_kind, ObObj &obj)
{
  int ret = OB_SUCCESS;
  const char *s = text.ptr();
  const int64_t len = text.length();
  if (common::ObNullType == col_type.get_type() || FCK_NULL == cell_kind) {
    obj.set_null();
  } else {
    switch (col_type.get_type()) {
      case common::ObIntType: {
        int64_t val = 0;
        if (!parse_int64_text(s, len, val)) {
          ret = OB_INVALID_DATA;
          LOG_WARN("failed to convert text to bigint", K(text), K(ret));
        } else {
          obj.set_int(val);
        }
        break;
      }
      case common::ObDoubleType: {
        double val = 0.0;
        if (!parse_double_text(s, len, val)) {
          ret = OB_INVALID_DATA;
          LOG_WARN("failed to convert text to double", K(text), K(ret));
        } else {
          obj.set_double(val);
        }
        break;
      }
      case common::ObTinyIntType: {
        bool val = false;
        if (!parse_bool_text(s, len, val)) {
          ret = OB_INVALID_DATA;
          LOG_WARN("failed to convert text to boolean", K(text), K(ret));
        } else {
          obj.set_bool(val);
        }
        break;
      }
      case common::ObDateType: {
        int32_t year = 0;
        int32_t mon = 0;
        int32_t mday = 0;
        int32_t hour = 0;
        int32_t minute = 0;
        int32_t sec = 0;
        int32_t usec = 0;
        int32_t ob_date = 0;
        if (parse_date_text(s, len, year, mon, mday)) {
          // ok
        } else if (parse_datetime_text(s, len, year, mon, mday, hour, minute, sec, usec)) {
          // a datetime text is also acceptable for a date column (time part dropped)
        } else {
          ret = OB_INVALID_DATA;
          LOG_WARN("failed to convert text to date", K(text), K(ret));
          break;
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(file_build_date(year, mon, mday, ob_date))) {
            LOG_WARN("failed to build date obj value", K(text), K(ret));
          } else {
            obj.set_date(ob_date);
          }
        }
        break;
      }
      case common::ObDateTimeType: {
        int32_t year = 0;
        int32_t mon = 0;
        int32_t mday = 0;
        int32_t hour = 0;
        int32_t minute = 0;
        int32_t sec = 0;
        int32_t usec = 0;
        int64_t ob_datetime = 0;
        if (parse_datetime_text(s, len, year, mon, mday, hour, minute, sec, usec)) {
          // ok
        } else if (parse_date_text(s, len, year, mon, mday)) {
          // a date text is also acceptable for a datetime column (00:00:00)
        } else {
          ret = OB_INVALID_DATA;
          LOG_WARN("failed to convert text to datetime", K(text), K(ret));
          break;
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(file_build_datetime(year, mon, mday, hour, minute, sec, usec, ob_datetime))) {
            LOG_WARN("failed to build datetime obj value", K(text), K(ret));
          } else {
            obj.set_datetime(ob_datetime);
          }
        }
        break;
      }
      case common::ObVarcharType: {
        obj.set_varchar(text);
        obj.meta_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        break;
      }
      default:
        ret = OB_INVALID_DATA;
        LOG_WARN("unsupported file table column type", K(col_type), K(ret));
        break;
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
