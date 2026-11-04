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

#include "common/object/ob_object.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{

/**
 * @brief Deterministic type inference shared by the csv and json file table
 * readers. The inference is a full scan of the sampled rows: every cell is
 * classified into a FileCellKind and the kinds of one column are merged with
 * the widening rules below.
 */
enum FileCellKind
{
  FCK_NONE = 0,     // no value of this column has been seen yet
  FCK_NULL,         // explicit null (null literal matched or json null)
  FCK_INT,          // fits into int64
  FCK_DOUBLE,       // numeric but not an int64 (or int64 overflow)
  FCK_BOOL,         // 'true'/'false', case insensitive
  FCK_DATE,         // strictly YYYY-MM-DD
  FCK_DATETIME,     // strictly YYYY-MM-DD[ HH:MM:SS[.ffffff]]
  FCK_STRING,       // anything else
  FCK_MAX,
};

// Classify one text cell. When the text equals the configured null literal
// (empty null_str_ also matches empty text) the kind is FCK_NULL.
// An int64 that overflows BIGINT is classified as FCK_DOUBLE.
int classify_file_cell_text(
    const common::ObString &text, const common::ObString &null_str, FileCellKind &kind);

// Merge the kinds seen in one column, with widening rules:
//   NULL    x T          -> T
//   INT     x INT        -> INT
//   INT     x DOUBLE     -> DOUBLE
//   BOOL    x BOOL       -> BOOL
//   DATE    x DATE       -> DATE
//   DATETIME x DATETIME  -> DATETIME
//   any other combination  -> STRING
// The returned kind is FCK_MAX when the merge is impossible (caller error).
int merge_file_cell_kind(FileCellKind &acc, FileCellKind incoming);

// Map a merged column kind to the column meta type:
//   INT -> BIGINT, DOUBLE -> DOUBLE, BOOL -> BOOLEAN, DATE -> DATE,
//   DATETIME -> DATETIME, STRING -> VARCHAR(UTF8MB4_BIN), NULL -> NULL type.
int file_cell_kind_to_type(FileCellKind kind, common::ObObjMeta &type);

// Convert one cell text to an ObObj of the inferred column type. The string
// buffer of a varchar obj points into text, the caller must keep text alive
// until the obj is consumed.
int file_cell_text_to_obj(const common::ObString &text, const common::ObObjMeta &col_type,
    const FileCellKind cell_kind, common::ObObj &obj);

// Typed value helpers shared by readers, string buffers come from the caller.
int file_varchar_to_obj(const common::ObString &v, common::ObObj &obj);
int file_int64_to_obj(int64_t v, common::ObObj &obj);
int file_double_to_obj(double v, common::ObObj &obj);
int file_bool_to_obj(bool v, common::ObObj &obj);
int file_date_to_obj(int32_t ob_date, common::ObObj &obj);
int file_datetime_to_obj(int64_t ob_datetime, common::ObObj &obj);

// Build an OB date/datetime integer from civil time parts, shared by the
// parquet timestamp conversions and the strict text parsing of csv/json.
// days_since_epoch follows the unix epoch (1970-01-01 is day 0).
int file_civil_from_days(int64_t days_since_epoch, int32_t &year, int32_t &mon, int32_t &mday);
int file_build_date(int32_t year, int32_t mon, int32_t mday, int32_t &ob_date);
int file_build_datetime(int32_t year, int32_t mon, int32_t mday, int32_t hour, int32_t min,
    int32_t sec, int32_t usec, int64_t &ob_datetime);

// Strict numeric text parsers shared by the csv and json readers:
//   int64: optional +/- followed by digits only, the whole text is consumed
//          and the value must fit into int64 (INT64_MIN/MAX included);
//   double: optional +/-, digits with an optional fraction and/or exponent,
//          the whole text is consumed ("inf"/"nan"/hex are rejected).
bool file_parse_int64_text(const char *s, int64_t len, int64_t &val);
bool file_parse_double_text(const char *s, int64_t len, double &val);

} // end namespace sql
} // end namespace oceanbase
