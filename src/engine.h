#pragma once

#include "parser.h"
#include "storage.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace mvdb {

struct Result {
  bool is_query = false;
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
  int64_t affected = 0;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
inline int column_index(const TableSchema& s, const std::string& name) {
  for (size_t i = 0; i < s.columns.size(); ++i) {
    if (s.columns[i].name == name) return static_cast<int>(i);
  }
  // MySQL column names are case-insensitive.
  std::string u = upper_copy(name);
  for (size_t i = 0; i < s.columns.size(); ++i) {
    if (upper_copy(s.columns[i].name) == u) return static_cast<int>(i);
  }
  throw DbError(1054, "Unknown column '" + name + "'", "42S22");
}

inline bool value_equal(const Value& a, const Value& b) {
  if (a.index() != b.index()) return false;
  if (const auto* p = std::get_if<int64_t>(&a)) {
    return *p == std::get<int64_t>(b);
  }
  if (const auto* p = std::get_if<std::string>(&a)) {
    return *p == std::get<std::string>(b);
  }
  if (const auto* p = std::get_if<Vector>(&a)) {
    return *p == std::get<Vector>(b);
  }
  return true;
}

inline bool cmp_i(int64_t a, int64_t b, CmpOp op) {
  switch (op) {
    case CmpOp::Eq: return a == b;
    case CmpOp::Ne: return a != b;
    case CmpOp::Lt: return a < b;
    case CmpOp::Le: return a <= b;
    case CmpOp::Gt: return a > b;
    case CmpOp::Ge: return a >= b;
  }
  return false;
}

inline bool cmp_d(double a, double b, CmpOp op) {
  switch (op) {
    case CmpOp::Eq: return a == b;
    case CmpOp::Ne: return a != b;
    case CmpOp::Lt: return a < b;
    case CmpOp::Le: return a <= b;
    case CmpOp::Gt: return a > b;
    case CmpOp::Ge: return a >= b;
  }
  return false;
}

inline bool cmp_s(const std::string& a, const std::string& b, CmpOp op) {
  int c = a.compare(b);
  switch (op) {
    case CmpOp::Eq: return c == 0;
    case CmpOp::Ne: return c != 0;
    case CmpOp::Lt: return c < 0;
    case CmpOp::Le: return c <= 0;
    case CmpOp::Gt: return c > 0;
    case CmpOp::Ge: return c >= 0;
  }
  return false;
}

inline bool parse_i64(const std::string& s, int64_t& out) {
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno == ERANGE || end != s.c_str() + s.size()) return false;
  out = static_cast<int64_t>(v);
  return true;
}

inline Value literal_to_value(const Column& c, const Expr& e) {
  switch (c.type) {
    case ColType::BigInt: {
      if (e.kind == ExprKind::Number) {
        if (e.is_integer) return e.inum;
        double d = e.num;
        if (!std::isfinite(d) || std::floor(d) != d ||
            d < -9223372036854775808.0 || d > 9223372036854775807.0) {
          throw DbError(1264, "Integer value out of range", "22003");
        }
        return static_cast<int64_t>(d);
      }
      if (e.kind == ExprKind::String) {
        int64_t v = 0;
        if (parse_i64(std::get<std::string>(e.value), v)) return v;
        throw DbError(1366, "Incorrect integer value", "22007");
      }
      throw DbError(1366, "Incorrect integer value", "22007");
    }
    case ColType::Varchar: {
      std::string s;
      if (e.kind == ExprKind::String) {
        s = std::get<std::string>(e.value);
      } else if (e.kind == ExprKind::Number) {
        s = e.is_integer ? std::to_string(e.inum) : format_float_compact(e.num);
      } else {
        throw DbError(1366, "Incorrect string value", "22007");
      }
      if (utf8_char_count(s) > static_cast<size_t>(c.len)) {
        throw DbError(1406, "Data too long for column '" + c.name + "'", "22001");
      }
      return s;
    }
    case ColType::Vector: {
      if (e.kind == ExprKind::Vector) {
        const Vector& v = std::get<Vector>(e.value);
        if (static_cast<int>(v.size()) != c.len) {
          throw DbError(1366, "Vector dimension mismatch for column '" + c.name + "'",
                        "22007");
        }
        return v;
      }
      if (e.kind == ExprKind::String) {
        return parse_vector_literal(std::get<std::string>(e.value), c.len);
      }
      throw DbError(1366, "Incorrect vector value", "22007");
    }
  }
  throw DbError(1105, "unreachable");
}

inline Value default_value(const Column& c) {
  switch (c.type) {
    case ColType::BigInt: return int64_t{0};
    case ColType::Varchar: return std::string{};
    case ColType::Vector: return Vector(static_cast<size_t>(c.len), 0.0f);
  }
  return int64_t{0};
}

struct CompiledCond {
  int col = -1;
  CmpOp op = CmpOp::Eq;
  int kind = 0;  // 0=int, 1=double, 2=string
  int64_t i = 0;
  double d = 0;
  std::string s;
};

inline std::vector<CompiledCond> compile_conditions(
    const TableSchema& schema, const std::vector<Condition>& conds) {
  std::vector<CompiledCond> out;
  out.reserve(conds.size());
  for (const auto& c : conds) {
    int ci = column_index(schema, c.column);
    const Column& col = schema.columns[ci];
    CompiledCond cc;
    cc.col = ci;
    cc.op = c.op;
    if (col.type == ColType::BigInt) {
      if (c.value->kind == ExprKind::Number) {
        if (c.value->is_integer) {
          cc.kind = 0;
          cc.i = c.value->inum;
        } else {
          cc.kind = 1;
          cc.d = c.value->num;
        }
      } else if (c.value->kind == ExprKind::String) {
        int64_t v = 0;
        if (!parse_i64(std::get<std::string>(c.value->value), v)) {
          throw DbError(1366, "Incorrect integer value", "22007");
        }
        cc.kind = 0;
        cc.i = v;
      } else {
        throw DbError(1366, "Incorrect integer value", "22007");
      }
    } else if (col.type == ColType::Varchar) {
      if (c.value->kind == ExprKind::String) {
        cc.kind = 2;
        cc.s = std::get<std::string>(c.value->value);
      } else if (c.value->kind == ExprKind::Number) {
        cc.kind = 2;
        cc.s = c.value->is_integer ? std::to_string(c.value->inum)
                                   : format_float_compact(c.value->num);
      } else {
        throw DbError(1366, "Incorrect string value", "22007");
      }
    } else {
      throw DbError(1064, "VECTOR columns cannot be compared", "42000");
    }
    out.push_back(std::move(cc));
  }
  return out;
}

inline bool matches(const Row& row, const std::vector<CompiledCond>& conds) {
  for (const auto& c : conds) {
    if (c.kind == 0) {
      if (!cmp_i(value_int(row[c.col]), c.i, c.op)) return false;
    } else if (c.kind == 1) {
      if (!cmp_d(static_cast<double>(value_int(row[c.col])), c.d, c.op)) return false;
    } else {
      if (!cmp_s(value_str(row[c.col]), c.s, c.op)) return false;
    }
  }
  return true;
}

#if defined(__x86_64__)
__attribute__((target("avx2,fma")))
inline double l2_distance_avx2(const float* a, const float* b, size_t n) {
  __m256d sum = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 fa = _mm_loadu_ps(a + i);
    __m128 fb = _mm_loadu_ps(b + i);
    __m256d da = _mm256_cvtps_pd(fa);
    __m256d db = _mm256_cvtps_pd(fb);
    __m256d d = _mm256_sub_pd(da, db);
    sum = _mm256_fmadd_pd(d, d, sum);
  }
  alignas(32) double tmp[4];
  _mm256_storeu_pd(tmp, sum);
  double s = tmp[0] + tmp[1] + tmp[2] + tmp[3];
  for (; i < n; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    s += d * d;
  }
  return std::sqrt(s);
}
#endif

inline double l2_distance(const Vector& a, const Vector& b) {
  if (a.size() != b.size()) {
    throw DbError(1366, "Vector dimension mismatch", "22007");
  }
#if defined(__x86_64__)
  static const bool has_avx2 =
      __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  if (has_avx2) return l2_distance_avx2(a.data(), b.data(), a.size());
#endif
  double sum = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return std::sqrt(sum);
}

struct SortSpec {
  enum class Kind { Int, Double, String } kind = Kind::Int;
  int col = -1;
  int vec_col = -1;
  Vector vec;
  bool is_distance = false;
  bool asc = true;
};

struct SortKey {
  int kind = 0;
  int64_t i = 0;
  double d = 0;
  std::string s;
};

inline int compare_key(const SortKey& a, const SortKey& b) {
  if (a.kind != b.kind) return a.kind < b.kind ? -1 : 1;
  if (a.kind == 0) return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
  if (a.kind == 1) return a.d < b.d ? -1 : (a.d > b.d ? 1 : 0);
  int c = a.s.compare(b.s);
  return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

struct Candidate {
  int64_t pk = 0;
  Row row;
  std::vector<SortKey> keys;
};

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------
class Database {
 public:
  Database(std::string dir, size_t cache_mb)
      : dir_(ensure_dir(std::move(dir))),
        catalog_(dir_),
        wal_(dir_ + "/wal.log"),
        cache_limit_(cache_mb * 1024ull * 1024ull) {
    catalog_.load();
    WalState st = wal_.load_and_repair();
    for (const auto& e : catalog_.tables()) {
      auto td = std::make_unique<TableData>(dir_, e.id, e.schema, cache_limit_);
      td->recover(st);
      tables_[e.id] = std::move(td);
    }
    next_txn_id_ = st.max_txn + 1;
    next_commit_id_ = st.max_commit + 1;
    if (next_txn_id_ == 0) next_txn_id_ = 1;
    if (next_commit_id_ == 0) next_commit_id_ = 1;
  }

  ~Database() { shutdown(); }

  void shutdown() {
    if (in_txn_) {
      try {
        rollback_txn();
      } catch (...) {
      }
    }
    tables_.clear();
  }

  void set_heap_enabled(bool v) { heap_enabled_ = v; }

  Result execute(const Statement& st) {
    if (auto* s = std::get_if<SelectStmt>(&st)) return exec_select(*s);
    if (auto* s = std::get_if<InsertStmt>(&st)) {
      return auto_commit([&] { return exec_insert(*s); });
    }
    if (auto* s = std::get_if<UpdateStmt>(&st)) {
      return auto_commit([&] { return exec_update(*s); });
    }
    if (auto* s = std::get_if<DeleteStmt>(&st)) {
      return auto_commit([&] { return exec_delete(*s); });
    }
    if (auto* s = std::get_if<CreateTableStmt>(&st)) return exec_create_table(*s);
    if (auto* s = std::get_if<CreateIndexStmt>(&st)) return exec_create_index(*s);
    if (auto* s = std::get_if<DropTableStmt>(&st)) return exec_drop_table(*s);
    if (auto* s = std::get_if<TxnStmt>(&st)) return exec_txn(*s);
    if (auto* s = std::get_if<SetStmt>(&st)) return exec_set(*s);
    if (auto* s = std::get_if<ShowStmt>(&st)) return exec_show(*s);
    throw DbError(1064, "Unsupported statement", "42000");
  }

 private:
  std::string dir_;
  Catalog catalog_;
  Wal wal_;
  size_t cache_limit_ = 64ull * 1024 * 1024;
  std::unordered_map<uint32_t, std::unique_ptr<TableData>> tables_;
  bool in_txn_ = false;
  uint64_t txn_id_ = 0;
  uint64_t next_txn_id_ = 1;
  uint64_t next_commit_id_ = 1;
  std::unordered_set<uint32_t> begun_;
  std::vector<uint32_t> touched_;
  std::string vector_mode_ = "exact";
  bool heap_enabled_ = true;

  static std::string ensure_dir(std::string d) {
    std::filesystem::create_directories(d);
    return d;
  }

  TableData& table(const std::string& name, uint32_t* id_out = nullptr) {
    TableEntry* e = catalog_.find(name);
    if (!e) {
      throw DbError(1146, "Table '" + name + "' doesn't exist", "42S02");
    }
    if (id_out) *id_out = e->id;
    auto it = tables_.find(e->id);
    if (it == tables_.end()) {
      auto td = std::make_unique<TableData>(dir_, e->id, e->schema, cache_limit_);
      it = tables_.emplace(e->id, std::move(td)).first;
    }
    return *it->second;
  }

  void touch(TableData& td, uint32_t id) {
    if (begun_.insert(id).second) {
      td.begin(txn_id_);
      touched_.push_back(id);
    }
  }

  template <typename F>
  Result auto_commit(F&& f) {
    if (in_txn_) return f();
    begin_txn();
    try {
      Result r = f();
      commit_txn();
      return r;
    } catch (...) {
      try {
        rollback_txn();
      } catch (...) {
      }
      throw;
    }
  }

  void begin_txn() {
    if (in_txn_) {
      throw DbError(1568, "Transaction already in progress", "25001");
    }
    txn_id_ = next_txn_id_++;
    wal_.append_begin(txn_id_);
    in_txn_ = true;
    begun_.clear();
    touched_.clear();
  }

  void commit_txn() {
    if (!in_txn_) return;
    uint64_t cid = next_commit_id_++;
    for (uint32_t id : touched_) {
      auto it = tables_.find(id);
      if (it != tables_.end()) it->second->fsync_data();
    }
    for (uint32_t id : touched_) {
      auto it = tables_.find(id);
      if (it != tables_.end()) it->second->commit_pending(cid);
    }
    wal_.append_commit(txn_id_, cid);
    wal_.sync();
    in_txn_ = false;
    begun_.clear();
    touched_.clear();
  }

  void rollback_txn() {
    if (!in_txn_) return;
    for (uint32_t id : touched_) {
      auto it = tables_.find(id);
      if (it != tables_.end()) it->second->rollback_pending();
    }
    wal_.append_abort(txn_id_);
    wal_.sync();
    in_txn_ = false;
    begun_.clear();
    touched_.clear();
  }

  Result exec_txn(const TxnStmt& st) {
    switch (st.kind) {
      case TxnStmt::Kind::Begin:
        begin_txn();
        break;
      case TxnStmt::Kind::Commit:
        commit_txn();
        break;
      case TxnStmt::Kind::Rollback:
        rollback_txn();
        break;
    }
    return Result{};
  }

  Result exec_set(const SetStmt& st) {
    std::string n = st.name;
    std::string v = upper_copy(st.value);
    if (n == "vector_mode" || n == "mode") {
      vector_mode_ = (v == "ANN") ? "ann" : "exact";
    } else if (n == "ann") {
      vector_mode_ = (v == "1" || v == "ON" || v == "TRUE" || v == "ANN") ? "ann"
                                                                       : "exact";
    }
    return Result{};
  }

  Result exec_create_table(const CreateTableStmt& st) {
    if (in_txn_) {
      throw DbError(1105, "DDL is not allowed inside a transaction");
    }
    if (catalog_.find(st.schema.name)) {
      if (st.if_not_exists) return Result{};
      throw DbError(1050, "Table '" + st.schema.name + "' already exists",
                    "42S01");
    }
    TableEntry& e = catalog_.create(st.schema);
    catalog_.save();
    tables_[e.id] =
        std::make_unique<TableData>(dir_, e.id, e.schema, cache_limit_);
    return Result{};
  }

  Result exec_create_index(const CreateIndexStmt& st) {
    if (in_txn_) {
      throw DbError(1105, "DDL is not allowed inside a transaction");
    }
    uint32_t id = 0;
    table(st.table, &id);
    TableEntry* e = catalog_.find(st.table);
    int ci = column_index(e->schema, st.column);
    if (e->schema.columns[ci].type != ColType::Vector) {
      throw DbError(1064, "A vector index must be created on a VECTOR column",
                    "42000");
    }
    for (const auto& ix : e->schema.indexes) {
      if (ix.name == st.name) {
        if (st.if_not_exists) return Result{};
        throw DbError(1061, "Duplicate key name '" + st.name + "'", "42000");
      }
    }
    e->schema.indexes.push_back(IndexDef{st.name, ci});
    catalog_.save();
    return Result{};
  }

  Result exec_drop_table(const DropTableStmt& st) {
    if (in_txn_) {
      throw DbError(1105, "DDL is not allowed inside a transaction");
    }
    TableEntry* e = catalog_.find(st.table);
    if (!e) {
      if (st.if_exists) return Result{};
      throw DbError(1146, "Table '" + st.table + "' doesn't exist", "42S02");
    }
    uint32_t id = e->id;
    tables_.erase(id);
    catalog_.drop(st.table);
    catalog_.save();
    std::filesystem::remove(dir_ + "/t_" + std::to_string(id) + ".dat");
    return Result{};
  }

  Result exec_show(const ShowStmt& st) {
    Result r;
    r.is_query = true;
    if (st.kind == ShowStmt::Kind::Tables) {
      r.columns = {"Tables"};
      std::vector<std::string> names;
      for (const auto& e : catalog_.tables()) names.push_back(e.schema.name);
      std::sort(names.begin(), names.end());
      for (const auto& n : names) r.rows.push_back({n});
      return r;
    }
    TableEntry* e = catalog_.find(st.table);
    if (!e) {
      throw DbError(1146, "Table '" + st.table + "' doesn't exist", "42S02");
    }
    r.columns = {"Field", "Type", "Null", "Key", "Default", "Extra"};
    for (const auto& c : e->schema.columns) {
      std::string type;
      switch (c.type) {
        case ColType::BigInt: type = "bigint"; break;
        case ColType::Varchar: type = "varchar(" + std::to_string(c.len) + ")"; break;
        case ColType::Vector: type = "vector(" + std::to_string(c.len) + ")"; break;
      }
      r.rows.push_back({c.name, type, "NO", c.primary ? "PRI" : "", "NULL", ""});
    }
    return r;
  }

  Result exec_insert(const InsertStmt& st) {
    uint32_t id = 0;
    TableData& td = table(st.table, &id);
    const TableSchema& schema = td.schema();
    touch(td, id);

    std::vector<int> map;
    if (st.columns.empty()) {
      for (size_t i = 0; i < schema.columns.size(); ++i) {
        map.push_back(static_cast<int>(i));
      }
    } else {
      std::unordered_set<int> seen;
      for (const auto& name : st.columns) {
        int ci = column_index(schema, name);
        if (!seen.insert(ci).second) {
          throw DbError(1110, "Column '" + name + "' specified twice", "42000");
        }
        map.push_back(ci);
      }
    }
    bool pk_present = false;
    for (int ci : map) {
      if (ci == schema.pk) pk_present = true;
    }
    if (!pk_present) {
      throw DbError(1364, "Field '" + schema.columns[schema.pk].name +
                              "' doesn't have a default value",
                    "HY000");
    }

    std::vector<Row> rows;
    std::unordered_set<int64_t> batch_pks;
    rows.reserve(st.rows.size());
    for (const auto& exprs : st.rows) {
      if (exprs.size() != map.size()) {
        throw DbError(1136, "Column count doesn't match value count", "21S01");
      }
      Row r(schema.columns.size());
      for (size_t i = 0; i < schema.columns.size(); ++i) {
        r[i] = default_value(schema.columns[i]);
      }
      for (size_t k = 0; k < map.size(); ++k) {
        r[map[k]] = literal_to_value(schema.columns[map[k]], *exprs[k]);
      }
      int64_t pk = value_int(r[schema.pk]);
      if (!batch_pks.insert(pk).second) {
        throw DbError(1062, "Duplicate entry '" + std::to_string(pk) +
                                "' for key 'PRIMARY'",
                      "23000");
      }
      if (td.visible_exists(pk)) {
        throw DbError(1062, "Duplicate entry '" + std::to_string(pk) +
                                "' for key 'PRIMARY'",
                      "23000");
      }
      rows.push_back(std::move(r));
    }

    size_t mark = td.pending_size();
    try {
      for (auto& r : rows) {
        int64_t pk = value_int(r[schema.pk]);
        td.append_pending(pk, std::move(r), false);
      }
    } catch (...) {
      td.rollback_to(mark);
      try {
        rollback_txn();
      } catch (...) {
      }
      throw;
    }
    Result res;
    res.affected = static_cast<int64_t>(rows.size());
    return res;
  }

  Result exec_update(const UpdateStmt& st) {
    uint32_t id = 0;
    TableData& td = table(st.table, &id);
    const TableSchema& schema = td.schema();
    touch(td, id);

    std::vector<std::pair<int, Value>> assigns;
    assigns.reserve(st.assigns.size());
    for (const auto& a : st.assigns) {
      int ci = column_index(schema, a.column);
      if (ci == schema.pk) {
        throw DbError(1064, "Primary key cannot be updated", "42000");
      }
      assigns.emplace_back(ci, literal_to_value(schema.columns[ci], *a.value));
    }
    auto conds = compile_conditions(schema, st.where);
    std::vector<int64_t> pks;
    td.scan_visible([&](int64_t pk, const Row& row) {
      if (matches(row, conds)) pks.push_back(pk);
    });

    size_t mark = td.pending_size();
    int64_t changed = 0;
    try {
      for (int64_t pk : pks) {
        Row row;
        if (!td.get_visible_row(pk, row)) continue;
        Row nrow = row;
        for (const auto& [ci, v] : assigns) nrow[ci] = v;
        bool diff = false;
        for (size_t i = 0; i < row.size(); ++i) {
          if (!value_equal(row[i], nrow[i])) {
            diff = true;
            break;
          }
        }
        if (!diff) continue;
        td.append_pending(pk, std::move(nrow), false);
        ++changed;
      }
    } catch (...) {
      td.rollback_to(mark);
      try {
        rollback_txn();
      } catch (...) {
      }
      throw;
    }
    Result res;
    res.affected = changed;
    return res;
  }

  Result exec_delete(const DeleteStmt& st) {
    uint32_t id = 0;
    TableData& td = table(st.table, &id);
    const TableSchema& schema = td.schema();
    touch(td, id);

    auto conds = compile_conditions(schema, st.where);
    std::vector<int64_t> pks;
    td.scan_visible([&](int64_t pk, const Row& row) {
      if (matches(row, conds)) pks.push_back(pk);
    });

    size_t mark = td.pending_size();
    try {
      for (int64_t pk : pks) td.append_pending(pk, Row{}, true);
    } catch (...) {
      td.rollback_to(mark);
      try {
        rollback_txn();
      } catch (...) {
      }
      throw;
    }
    Result res;
    res.affected = static_cast<int64_t>(pks.size());
    return res;
  }

  SortSpec make_distance_spec(const Expr& e, const TableSchema& schema,
                              bool asc) {
    int ci = column_index(schema, e.args[0]->text);
    if (schema.columns[ci].type != ColType::Vector) {
      throw DbError(1064, "L2_DISTANCE requires a VECTOR column", "42000");
    }
    const Vector& v = std::get<Vector>(e.args[1]->value);
    if (static_cast<int>(v.size()) != schema.columns[ci].len) {
      throw DbError(1366, "Vector dimension mismatch", "22007");
    }
    SortSpec sp;
    sp.kind = SortSpec::Kind::Double;
    sp.vec_col = ci;
    sp.vec = v;
    sp.is_distance = true;
    sp.asc = asc;
    return sp;
  }

  std::vector<SortSpec> resolve_order_specs(const SelectStmt& st,
                                            const TableSchema& schema) {
    std::vector<SortSpec> out;
    for (const auto& oi : st.order) {
      const Expr* e = oi.expr.get();
      if (e->kind == ExprKind::Column) {
        const Expr* target = nullptr;
        for (const auto& it : st.items) {
          if (!it.alias.empty() && it.alias == e->text) {
            target = it.expr.get();
            break;
          }
        }
        if (target) e = target;
        if (e->kind == ExprKind::Column) {
          int ci = column_index(schema, e->text);
          SortSpec sp;
          sp.asc = oi.asc;
          if (schema.columns[ci].type == ColType::BigInt) {
            sp.kind = SortSpec::Kind::Int;
            sp.col = ci;
          } else if (schema.columns[ci].type == ColType::Varchar) {
            sp.kind = SortSpec::Kind::String;
            sp.col = ci;
          } else {
            throw DbError(1064, "Cannot ORDER BY a VECTOR column", "42000");
          }
          out.push_back(std::move(sp));
        } else if (e->kind == ExprKind::L2Distance) {
          out.push_back(make_distance_spec(*e, schema, oi.asc));
        } else {
          throw DbError(1064, "Unsupported ORDER BY expression", "42000");
        }
      } else if (e->kind == ExprKind::L2Distance) {
        out.push_back(make_distance_spec(*e, schema, oi.asc));
      } else {
        throw DbError(1064, "Unsupported ORDER BY expression", "42000");
      }
    }
    return out;
  }

  static SortKey key_for(const SortSpec& sp, const Row& row) {
    SortKey k;
    if (sp.is_distance) {
      k.kind = 1;
      k.d = l2_distance(value_vec(row[sp.vec_col]), sp.vec);
    } else if (sp.kind == SortSpec::Kind::Int) {
      k.kind = 0;
      k.i = value_int(row[sp.col]);
    } else {
      k.kind = 2;
      k.s = value_str(row[sp.col]);
    }
    return k;
  }

  std::string eval_string(const Expr& e, const Row& row,
                          const TableSchema& schema) {
    switch (e.kind) {
      case ExprKind::Column: {
        int ci = column_index(schema, e.text);
        return format_value(row[ci]);
      }
      case ExprKind::Number:
        return e.is_integer ? std::to_string(e.inum) : format_float_compact(e.num);
      case ExprKind::String:
        return std::get<std::string>(e.value);
      case ExprKind::Vector:
        return format_value(e.value);
      case ExprKind::L2Distance: {
        int ci = column_index(schema, e.args[0]->text);
        if (schema.columns[ci].type != ColType::Vector) {
          throw DbError(1064, "L2_DISTANCE requires a VECTOR column", "42000");
        }
        const Vector& v = std::get<Vector>(e.args[1]->value);
        if (static_cast<int>(v.size()) != schema.columns[ci].len) {
          throw DbError(1366, "Vector dimension mismatch", "22007");
        }
        return format_float(l2_distance(value_vec(row[ci]), v));
      }
    }
    return {};
  }

  Result exec_select(const SelectStmt& st) {
    if (st.no_from) {
      if (st.star) {
        throw DbError(1064, "SELECT * requires FROM", "42000");
      }
      if (!st.where.empty() || !st.order.empty()) {
        throw DbError(1064, "WHERE/ORDER BY requires FROM", "42000");
      }
      Result r;
      r.is_query = true;
      for (const auto& it : st.items) {
        r.columns.push_back(it.alias.empty() ? it.expr->text : it.alias);
      }
      if (!st.has_limit || st.limit > 0) {
        TableSchema empty_schema;
        Row empty;
        std::vector<std::string> out;
        out.reserve(st.items.size());
        for (const auto& it : st.items) {
          out.push_back(eval_string(*it.expr, empty, empty_schema));
        }
        r.rows.push_back(std::move(out));
      }
      return r;
    }
    TableData& td = table(st.table);
    const TableSchema& schema = td.schema();
    auto conds = compile_conditions(schema, st.where);
    auto specs = resolve_order_specs(st, schema);

    std::vector<Candidate> cands;
    auto better = [&](const Candidate& a, const Candidate& b) {
      for (size_t i = 0; i < specs.size(); ++i) {
        int c = compare_key(a.keys[i], b.keys[i]);
        if (c != 0) return specs[i].asc ? (c < 0) : (c > 0);
      }
      return a.pk < b.pk;
    };

    const bool empty_limit = st.has_limit && st.limit == 0;
    bool use_heap =
        heap_enabled_ && st.has_limit && st.limit > 0 && !specs.empty();
    size_t keep = 0;
    if (use_heap) {
      keep = static_cast<size_t>(st.offset) + static_cast<size_t>(st.limit);
      if (keep < static_cast<size_t>(st.limit) || keep == 0) use_heap = false;
    }
    std::vector<Candidate> heap;
    if (use_heap) heap.reserve(std::min<size_t>(keep, 4096));

    if (!empty_limit) {
      td.scan_visible([&](int64_t pk, const Row& row) {
        if (!matches(row, conds)) return;
        Candidate c;
        c.pk = pk;
        c.keys.reserve(specs.size());
        for (const auto& sp : specs) c.keys.push_back(key_for(sp, row));
        if (!use_heap) {
          c.row = row;
          cands.push_back(std::move(c));
          return;
        }
        if (heap.size() < keep) {
          c.row = row;
          heap.push_back(std::move(c));
          std::push_heap(heap.begin(), heap.end(), better);
        } else if (better(c, heap.front())) {
          c.row = row;
          std::pop_heap(heap.begin(), heap.end(), better);
          heap.back() = std::move(c);
          std::push_heap(heap.begin(), heap.end(), better);
        }
      });
    }

    if (use_heap) {
      cands = std::move(heap);
      std::sort(cands.begin(), cands.end(), better);
    } else if (specs.empty()) {
      std::sort(cands.begin(), cands.end(),
                [](const Candidate& a, const Candidate& b) { return a.pk < b.pk; });
    } else {
      std::sort(cands.begin(), cands.end(), better);
    }

    size_t start = 0;
    size_t end = cands.size();
    if (st.has_limit) {
      start = std::min(static_cast<size_t>(st.offset), cands.size());
      if (st.limit >= 0) {
        size_t cnt = static_cast<size_t>(st.limit);
        end = (cnt > cands.size() - start) ? cands.size() : start + cnt;
      }
    }

    Result r;
    r.is_query = true;
    if (st.star) {
      for (const auto& c : schema.columns) r.columns.push_back(c.name);
      for (size_t i = start; i < end; ++i) {
        std::vector<std::string> out;
        out.reserve(cands[i].row.size());
        for (const auto& v : cands[i].row) out.push_back(format_value(v));
        r.rows.push_back(std::move(out));
      }
    } else {
      for (const auto& it : st.items) {
        r.columns.push_back(it.alias.empty() ? it.expr->text : it.alias);
      }
      for (size_t i = start; i < end; ++i) {
        std::vector<std::string> out;
        out.reserve(st.items.size());
        for (const auto& it : st.items) {
          out.push_back(eval_string(*it.expr, cands[i].row, schema));
        }
        r.rows.push_back(std::move(out));
      }
    }
    return r;
  }
};

}  // namespace mvdb
