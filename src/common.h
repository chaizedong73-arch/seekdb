#pragma once

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace mvdb {

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------
struct DbError : public std::exception {
  int code;
  std::string state;
  std::string message;
  DbError(int c, std::string m, std::string s = "HY000")
      : code(c), state(std::move(s)), message(std::move(m)) {}
  const char* what() const noexcept override { return message.c_str(); }
};

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------
enum class ColType { BigInt, Varchar, Vector };

struct Column {
  std::string name;
  ColType type = ColType::BigInt;
  int len = 0;  // VARCHAR character length or VECTOR dimension
  bool primary = false;
};

struct IndexDef {
  std::string name;
  int column = -1;
};

struct TableSchema {
  std::string name;
  std::vector<Column> columns;
  int pk = -1;
  int vec = -1;
  std::vector<IndexDef> indexes;
};

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------
using Vector = std::vector<float>;
using Value = std::variant<std::monostate, int64_t, double, std::string, Vector>;

inline bool is_null(const Value& v) {
  return std::holds_alternative<std::monostate>(v);
}

inline int64_t value_int(const Value& v) {
  if (const auto* p = std::get_if<int64_t>(&v)) return *p;
  throw DbError(1366, "Incorrect integer value", "22007");
}

inline const std::string& value_str(const Value& v) {
  if (const auto* p = std::get_if<std::string>(&v)) return *p;
  throw DbError(1366, "Incorrect string value", "22007");
}

inline const Vector& value_vec(const Value& v) {
  if (const auto* p = std::get_if<Vector>(&v)) return *p;
  throw DbError(1366, "Incorrect vector value", "22007");
}

// VARCHAR length is measured in Unicode code points, not bytes. UTF-8 keeps
// code-point order under byte-wise comparison, so std::string's operator< is
// already the required "Unicode code point order" comparison.
inline size_t utf8_char_count(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80) ++n;
  }
  return n;
}

inline std::string trim_copy(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

inline std::string upper_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// Parse "[1, 2, 3]" (with or without the brackets) into finite float32
// values. dim > 0 enforces an exact dimension; dim == 0 means "any dimension"
// and is used while parsing a literal before the target column is known.
inline Vector parse_vector_literal(const std::string& text, int dim) {
  std::string s = trim_copy(text);
  if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
    s = s.substr(1, s.size() - 2);
  }
  Vector out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() &&
           (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) {
      ++i;
    }
    if (i >= s.size()) break;
    const char* begin = s.c_str() + i;
    char* end = nullptr;
    errno = 0;
    float f = std::strtof(begin, &end);
    if (end == begin) {
      throw DbError(1366, "Invalid vector literal", "22007");
    }
    if (errno == ERANGE || !std::isfinite(f)) {
      throw DbError(1366, "Vector value is not a finite float32", "22007");
    }
    out.push_back(f);
    i = static_cast<size_t>(end - s.c_str());
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i < s.size() && s[i] != ',') {
      throw DbError(1366, "Invalid vector literal", "22007");
    }
    if (i < s.size()) ++i;
  }
  if (dim > 0 && static_cast<int>(out.size()) != dim) {
    throw DbError(1366, "Vector dimension mismatch", "22007");
  }
  return out;
}

inline std::string format_float(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6f", v);
  return std::string(buf);
}

inline std::string format_float_compact(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return std::string(buf);
}

inline std::string format_value(const Value& v) {
  if (std::holds_alternative<std::monostate>(v)) return "NULL";
  if (const auto* p = std::get_if<int64_t>(&v)) return std::to_string(*p);
  if (const auto* p = std::get_if<double>(&v)) return format_float_compact(*p);
  if (const auto* p = std::get_if<std::string>(&v)) return *p;
  const auto& vec = std::get<Vector>(v);
  std::string out = "[";
  for (size_t i = 0; i < vec.size(); ++i) {
    if (i) out += ",";
    out += format_float_compact(vec[i]);
  }
  out += "]";
  return out;
}

inline std::string col_type_name(ColType t) {
  switch (t) {
    case ColType::BigInt: return "BIGINT";
    case ColType::Varchar: return "VARCHAR";
    case ColType::Vector: return "VECTOR";
  }
  return "UNKNOWN";
}

inline void validate_schema(const TableSchema& s) {
  if (s.name.empty()) throw DbError(1064, "Table name is empty", "42000");
  if (s.columns.empty()) throw DbError(1064, "Table must have columns", "42000");
  int pk_count = 0;
  int vec_count = 0;
  for (const auto& c : s.columns) {
    if (c.name.empty()) throw DbError(1064, "Column name is empty", "42000");
    if (c.primary) {
      if (c.type != ColType::BigInt) {
        throw DbError(1064, "PRIMARY KEY must be BIGINT", "42000");
      }
      ++pk_count;
    }
    if (c.type == ColType::Vector) {
      ++vec_count;
      if (c.len <= 0) throw DbError(1064, "VECTOR dimension must be positive", "42000");
    }
    if (c.type == ColType::Varchar && c.len <= 0) {
      throw DbError(1064, "VARCHAR length must be positive", "42000");
    }
  }
  if (pk_count != 1) {
    throw DbError(1064, "Exactly one BIGINT primary key is required", "42000");
  }
  if (vec_count > 1) {
    throw DbError(1064, "At most one VECTOR column is allowed", "42000");
  }
  for (size_t i = 0; i < s.columns.size(); ++i) {
    for (size_t j = i + 1; j < s.columns.size(); ++j) {
      if (upper_copy(s.columns[i].name) == upper_copy(s.columns[j].name)) {
        throw DbError(1060,
                      "Duplicate column name '" + s.columns[i].name + "'",
                      "42S21");
      }
    }
  }
}

}  // namespace mvdb
