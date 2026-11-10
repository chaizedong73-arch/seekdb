#pragma once

#include "common.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mvdb {

using Row = std::vector<Value>;

// ---------------------------------------------------------------------------
// Little-endian encoding helpers (the evaluation target is x86_64)
// ---------------------------------------------------------------------------
inline void put_u8(std::string& b, uint8_t v) { b.push_back(static_cast<char>(v)); }
inline void put_u32(std::string& b, uint32_t v) {
  b.append(reinterpret_cast<const char*>(&v), sizeof(v));
}
inline void put_u64(std::string& b, uint64_t v) {
  b.append(reinterpret_cast<const char*>(&v), sizeof(v));
}
inline void put_i64(std::string& b, int64_t v) {
  b.append(reinterpret_cast<const char*>(&v), sizeof(v));
}
inline void put_float(std::string& b, float v) {
  b.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

inline uint32_t get_u32(const char* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline uint64_t get_u64(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline int64_t get_i64(const char* p) {
  int64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline float get_float(const char* p) {
  float v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline uint32_t crc32_bytes(const void* data, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    init = true;
  }
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

inline void write_all(int fd, const void* data, size_t n) {
  const char* p = static_cast<const char*>(data);
  size_t off = 0;
  while (off < n) {
    ssize_t w = ::write(fd, p + off, n - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw DbError(1105, std::string("write failed: ") + std::strerror(errno));
    }
    off += static_cast<size_t>(w);
  }
}

inline void pwrite_all(int fd, const void* data, size_t n, uint64_t off) {
  const char* p = static_cast<const char*>(data);
  size_t done = 0;
  while (done < n) {
    ssize_t w = ::pwrite(fd, p + done, n - done, static_cast<off_t>(off + done));
    if (w < 0) {
      if (errno == EINTR) continue;
      throw DbError(1105, std::string("pwrite failed: ") + std::strerror(errno));
    }
    done += static_cast<size_t>(w);
  }
}

inline void pread_all(int fd, void* data, size_t n, uint64_t off) {
  char* p = static_cast<char*>(data);
  size_t done = 0;
  while (done < n) {
    ssize_t r = ::pread(fd, p + done, n - done, static_cast<off_t>(off + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      throw DbError(1105, std::string("pread failed: ") + std::strerror(errno));
    }
    if (r == 0) {
      throw DbError(1105, "unexpected end of file");
    }
    done += static_cast<size_t>(r);
  }
}

inline void fsync_dir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
}

inline void write_file_atomic(const std::string& path, const std::string& data) {
  std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    throw DbError(1105, std::string("open failed: ") + std::strerror(errno));
  }
  try {
    write_all(fd, data.data(), data.size());
    ::fsync(fd);
  } catch (...) {
    ::close(fd);
    throw;
  }
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    throw DbError(1105, std::string("rename failed: ") + std::strerror(errno));
  }
  fsync_dir(std::filesystem::path(path).parent_path().string());
}

// ---------------------------------------------------------------------------
// Row encoding
// ---------------------------------------------------------------------------
inline std::string encode_row(const TableSchema& s, const Row& row) {
  if (row.size() != s.columns.size()) {
    throw DbError(1105, "row width does not match schema");
  }
  std::string b;
  for (size_t i = 0; i < s.columns.size(); ++i) {
    const Column& c = s.columns[i];
    const Value& v = row[i];
    switch (c.type) {
      case ColType::BigInt:
        put_i64(b, value_int(v));
        break;
      case ColType::Varchar: {
        const std::string& str = value_str(v);
        if (str.size() > 0xFFFFFFFFull) {
          throw DbError(1406, "Data too long for column '" + c.name + "'",
                        "22001");
        }
        put_u32(b, static_cast<uint32_t>(str.size()));
        b += str;
        break;
      }
      case ColType::Vector: {
        const Vector& vec = value_vec(v);
        if (static_cast<int>(vec.size()) != c.len) {
          throw DbError(1366, "Vector dimension mismatch for column '" + c.name + "'",
                        "22007");
        }
        for (float f : vec) put_float(b, f);
        break;
      }
    }
  }
  return b;
}

inline Row decode_row(const TableSchema& s, const char* p, uint32_t n) {
  Row row;
  row.reserve(s.columns.size());
  uint32_t off = 0;
  auto need = [&](uint32_t k) {
    if (off + k > n) throw DbError(1105, "corrupt row record");
  };
  for (const Column& c : s.columns) {
    switch (c.type) {
      case ColType::BigInt:
        need(8);
        row.push_back(get_i64(p + off));
        off += 8;
        break;
      case ColType::Varchar: {
        need(4);
        uint32_t len = get_u32(p + off);
        off += 4;
        need(len);
        row.push_back(std::string(p + off, len));
        off += len;
        break;
      }
      case ColType::Vector: {
        uint32_t bytes = static_cast<uint32_t>(c.len) * 4u;
        need(bytes);
        Vector v(c.len);
        for (int i = 0; i < c.len; ++i) v[i] = get_float(p + off + i * 4);
        row.push_back(std::move(v));
        off += bytes;
        break;
      }
    }
  }
  if (off != n) throw DbError(1105, "corrupt row record (trailing bytes)");
  return row;
}

inline size_t row_memory_bytes(const Row& r) {
  size_t n = sizeof(Row);
  for (const auto& v : r) {
    if (const auto* s = std::get_if<std::string>(&v)) n += s->size();
    if (const auto* vec = std::get_if<Vector>(&v)) n += vec->size() * sizeof(float);
  }
  return n;
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------
struct TableEntry {
  uint32_t id = 0;
  TableSchema schema;
};

class Catalog {
 public:
  explicit Catalog(std::string dir) : dir_(std::move(dir)) {
    path_ = dir_ + "/catalog.meta";
  }

  void load() {
    tables_.clear();
    by_name_.clear();
    next_id_ = 1;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return;
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (data.size() < 9) {
      if (!data.empty()) throw DbError(1105, "corrupt catalog");
      return;
    }
    size_t off = 0;
    if (data.compare(0, 5, "MVDB1") != 0) throw DbError(1105, "bad catalog magic");
    off = 5;
    uint32_t count = get_u32(data.data() + off);
    off += 4;
    for (uint32_t t = 0; t < count; ++t) {
      auto rd_u32 = [&]() {
        if (off + 4 > data.size()) throw DbError(1105, "corrupt catalog");
        uint32_t v = get_u32(data.data() + off);
        off += 4;
        return v;
      };
      auto rd_str = [&]() {
        uint32_t n = rd_u32();
        if (off + n > data.size()) throw DbError(1105, "corrupt catalog");
        std::string s(data.data() + off, n);
        off += n;
        return s;
      };
      TableEntry e;
      e.id = rd_u32();
      e.schema.name = rd_str();
      uint32_t ncols = rd_u32();
      int32_t pk = static_cast<int32_t>(rd_u32());
      int32_t vec = static_cast<int32_t>(rd_u32());
      e.schema.pk = pk;
      e.schema.vec = vec;
      for (uint32_t c = 0; c < ncols; ++c) {
        Column col;
        col.name = rd_str();
        uint32_t type = rd_u32();
        col.type = static_cast<ColType>(type);
        col.len = static_cast<int32_t>(rd_u32());
        col.primary = rd_u32() != 0;
        e.schema.columns.push_back(std::move(col));
      }
      uint32_t nidx = rd_u32();
      for (uint32_t i = 0; i < nidx; ++i) {
        IndexDef ix;
        ix.name = rd_str();
        ix.column = static_cast<int32_t>(rd_u32());
        e.schema.indexes.push_back(std::move(ix));
      }
      next_id_ = std::max(next_id_, e.id + 1);
      by_name_[e.schema.name] = tables_.size();
      tables_.push_back(std::move(e));
    }
  }

  void save() {
    std::string data = "MVDB1";
    put_u32(data, static_cast<uint32_t>(tables_.size()));
    for (const auto& e : tables_) {
      put_u32(data, e.id);
      put_u32(data, static_cast<uint32_t>(e.schema.name.size()));
      data += e.schema.name;
      put_u32(data, static_cast<uint32_t>(e.schema.columns.size()));
      put_u32(data, static_cast<uint32_t>(e.schema.pk));
      put_u32(data, static_cast<uint32_t>(e.schema.vec));
      for (const auto& c : e.schema.columns) {
        put_u32(data, static_cast<uint32_t>(c.name.size()));
        data += c.name;
        put_u32(data, static_cast<uint32_t>(c.type));
        put_u32(data, static_cast<uint32_t>(c.len));
        put_u32(data, c.primary ? 1u : 0u);
      }
      put_u32(data, static_cast<uint32_t>(e.schema.indexes.size()));
      for (const auto& ix : e.schema.indexes) {
        put_u32(data, static_cast<uint32_t>(ix.name.size()));
        data += ix.name;
        put_u32(data, static_cast<uint32_t>(ix.column));
      }
    }
    write_file_atomic(path_, data);
  }

  TableEntry* find(const std::string& name) {
    auto it = by_name_.find(name);
    if (it == by_name_.end()) return nullptr;
    return &tables_[it->second];
  }

  const std::vector<TableEntry>& tables() const { return tables_; }

  TableEntry& create(TableSchema schema) {
    TableEntry e;
    e.id = next_id_++;
    e.schema = std::move(schema);
    by_name_[e.schema.name] = tables_.size();
    tables_.push_back(std::move(e));
    return tables_.back();
  }

  bool drop(const std::string& name) {
    auto it = by_name_.find(name);
    if (it == by_name_.end()) return false;
    size_t idx = it->second;
    tables_.erase(tables_.begin() + static_cast<long>(idx));
    by_name_.clear();
    for (size_t i = 0; i < tables_.size(); ++i) {
      by_name_[tables_[i].schema.name] = i;
    }
    return true;
  }

 private:
  std::string dir_;
  std::string path_;
  std::vector<TableEntry> tables_;
  std::unordered_map<std::string, size_t> by_name_;
  uint32_t next_id_ = 1;
};

// ---------------------------------------------------------------------------
// Write-ahead log
// ---------------------------------------------------------------------------
struct WalState {
  std::unordered_set<uint64_t> committed;
  std::unordered_set<uint64_t> aborted;
  std::unordered_map<uint64_t, uint64_t> commit_id;
  uint64_t max_txn = 0;
  uint64_t max_commit = 0;
};

class Wal {
 public:
  explicit Wal(std::string path) : path_(std::move(path)) {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) {
      throw DbError(1105, std::string("open wal failed: ") + std::strerror(errno));
    }
  }
  ~Wal() {
    if (fd_ >= 0) ::close(fd_);
  }

  WalState load_and_repair() {
    WalState st;
    struct stat sb {};
    if (::fstat(fd_, &sb) != 0) {
      throw DbError(1105, std::string("fstat wal failed: ") + std::strerror(errno));
    }
    uint64_t size = static_cast<uint64_t>(sb.st_size);
    uint64_t off = 0;
    while (off + 28 <= size) {
      char hdr[28];
      pread_all(fd_, hdr, 28, off);
      uint32_t len = get_u32(hdr);
      uint32_t crc = get_u32(hdr + 4);
      if (len < 28 || len > 1024 * 1024) break;
      if (off + len > size) break;
      std::string rec(len, '\0');
      std::memcpy(rec.data(), hdr, 28);
      if (len > 28) pread_all(fd_, rec.data() + 28, len - 28, off + 28);
      if (crc32_bytes(rec.data() + 8, len - 8) != crc) break;
      uint8_t type = static_cast<uint8_t>(rec[8]);
      uint64_t txn = get_u64(rec.data() + 12);
      uint64_t commit_id = get_u64(rec.data() + 20);
      st.max_txn = std::max(st.max_txn, txn);
      if (type == 1) {  // BEGIN
        // nothing else
      } else if (type == 2) {  // COMMIT
        st.committed.insert(txn);
        st.commit_id[txn] = commit_id;
        st.max_commit = std::max(st.max_commit, commit_id);
        st.max_txn = std::max(st.max_txn, txn);
      } else if (type == 3) {  // ABORT
        st.aborted.insert(txn);
      }
      off += len;
    }
    if (off < size) {
      if (::ftruncate(fd_, static_cast<off_t>(off)) != 0) {
        // A torn tail is ignored either way; repair is best-effort.
      }
      ::fdatasync(fd_);
    }
    return st;
  }

  void append_begin(uint64_t txn) { append(1, txn, 0); }
  void append_commit(uint64_t txn, uint64_t commit_id) { append(2, txn, commit_id); }
  void append_abort(uint64_t txn) { append(3, txn, 0); }

  void sync() {
    if (::fdatasync(fd_) != 0) {
      throw DbError(1105, std::string("wal sync failed: ") + std::strerror(errno));
    }
  }

  int fd() const { return fd_; }

 private:
  int fd_ = -1;
  std::string path_;

  void append(uint8_t type, uint64_t txn, uint64_t commit_id) {
    std::string rec;
    put_u32(rec, 28);
    put_u32(rec, 0);
    put_u8(rec, type);
    put_u8(rec, 0);
    put_u8(rec, 0);
    put_u8(rec, 0);
    put_u64(rec, txn);
    put_u64(rec, commit_id);
    uint32_t crc = crc32_bytes(rec.data() + 8, rec.size() - 8);
    std::memcpy(rec.data() + 4, &crc, 4);
    write_all(fd_, rec.data(), rec.size());
  }
};

// ---------------------------------------------------------------------------
// Table data: append-only row versions + in-memory PK map
// ---------------------------------------------------------------------------
struct Version {
  int64_t pk = 0;
  uint64_t offset = 0;
  uint32_t len = 0;
  uint64_t txn_id = 0;
  uint64_t commit_id = 0;
  bool deleted = false;
};

struct PendingVersion {
  Version meta;
  Row row;
};

class TableData {
 public:
  TableData(std::string dir, uint32_t id, TableSchema schema, size_t cache_limit)
      : dir_(std::move(dir)),
        schema_(std::move(schema)),
        cache_limit_(cache_limit) {
    path_ = dir_ + "/t_" + std::to_string(id) + ".dat";
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
      throw DbError(1105, std::string("open table failed: ") + std::strerror(errno));
    }
  }

  ~TableData() {
    if (fd_ >= 0) ::close(fd_);
  }

  const TableSchema& schema() const { return schema_; }

  void recover(const WalState& wal) {
    struct stat sb {};
    if (::fstat(fd_, &sb) != 0) {
      throw DbError(1105, std::string("fstat table failed: ") + std::strerror(errno));
    }
    uint64_t size = static_cast<uint64_t>(sb.st_size);
    uint64_t off = 0;
    while (off + 28 <= size) {
      char hdr[28];
      pread_all(fd_, hdr, 28, off);
      uint32_t len = get_u32(hdr);
      uint32_t crc = get_u32(hdr + 4);
      if (len < 28 || len > (1u << 30)) break;
      if (off + len > size) break;
      std::string rec(len, '\0');
      std::memcpy(rec.data(), hdr, 28);
      if (len > 28) pread_all(fd_, rec.data() + 28, len - 28, off + 28);
      if (crc32_bytes(rec.data() + 8, len - 8) != crc) break;
      uint64_t txn = get_u64(rec.data() + 8);
      int64_t pk = get_i64(rec.data() + 16);
      uint8_t flags = static_cast<uint8_t>(rec[24]);
      auto it = wal.commit_id.find(txn);
      if (it != wal.commit_id.end() && !wal.aborted.count(txn)) {
        Version v;
        v.pk = pk;
        v.offset = off;
        v.len = len;
        v.txn_id = txn;
        v.commit_id = it->second;
        v.deleted = (flags & 1u) != 0;
        uint32_t idx = static_cast<uint32_t>(versions_.size());
        versions_.push_back(v);
        auto lit = latest_.find(pk);
        if (lit == latest_.end()) {
          latest_[pk] = idx;
        } else {
          const Version& old = versions_[lit->second];
          if (v.commit_id > old.commit_id ||
              (v.commit_id == old.commit_id && v.offset > old.offset)) {
            latest_[pk] = idx;
          }
        }
      }
      off += len;
    }
    valid_end_ = off;
    if (off < size) {
      if (::ftruncate(fd_, static_cast<off_t>(off)) != 0) {
        // Best-effort repair of a torn tail.
      }
      ::fdatasync(fd_);
    }
    build_cache();
  }

  void init_empty_cache() {
    if (!cache_valid_ && cache_.empty()) {
      cache_valid_ = true;
      cache_bytes_ = 0;
    }
  }

  void begin(uint64_t txn_id) {
    active_txn_ = txn_id;
    pending_.clear();
    pending_latest_.clear();
  }

  uint32_t append_pending(int64_t pk, Row row, bool deleted) {
    std::string payload;
    if (!deleted) payload = encode_row(schema_, row);
    std::string rec;
    put_u32(rec, 0);
    put_u32(rec, 0);
    put_u64(rec, active_txn_);
    put_i64(rec, pk);
    put_u8(rec, deleted ? 1u : 0u);
    put_u8(rec, 0);
    put_u8(rec, 0);
    put_u8(rec, 0);
    rec += payload;
    uint32_t len = static_cast<uint32_t>(rec.size());
    std::memcpy(rec.data(), &len, 4);
    uint32_t crc = crc32_bytes(rec.data() + 8, rec.size() - 8);
    std::memcpy(rec.data() + 4, &crc, 4);
    pwrite_all(fd_, rec.data(), rec.size(), valid_end_);

    PendingVersion pv;
    pv.meta.pk = pk;
    pv.meta.offset = valid_end_;
    pv.meta.len = len;
    pv.meta.txn_id = active_txn_;
    pv.meta.deleted = deleted;
    pv.row = deleted ? Row{} : std::move(row);
    valid_end_ += len;
    uint32_t idx = static_cast<uint32_t>(pending_.size());
    pending_.push_back(std::move(pv));
    pending_latest_[pk] = idx;
    return idx;
  }

  void commit_pending(uint64_t commit_id) {
    for (const auto& pv : pending_) {
      Version v = pv.meta;
      v.commit_id = commit_id;
      uint32_t idx = static_cast<uint32_t>(versions_.size());
      versions_.push_back(v);
      auto lit = latest_.find(v.pk);
      if (lit == latest_.end()) {
        latest_[v.pk] = idx;
      } else {
        const Version& old = versions_[lit->second];
        if (v.commit_id > old.commit_id ||
            (v.commit_id == old.commit_id && v.offset > old.offset)) {
          latest_[v.pk] = idx;
        }
      }
    }
    if (cache_valid_) {
      for (const auto& [pk, idx] : pending_latest_) {
        const PendingVersion& pv = pending_[idx];
        auto it = cache_index_.find(pk);
        if (pv.meta.deleted) {
          if (it != cache_index_.end()) {
            uint32_t pos = it->second;
            cache_bytes_ -= row_memory_bytes(cache_[pos]);
            uint32_t last = static_cast<uint32_t>(cache_.size() - 1);
            if (pos != last) {
              cache_[pos] = std::move(cache_[last]);
              cache_pks_[pos] = cache_pks_[last];
              cache_index_[cache_pks_[pos]] = pos;
            }
            cache_.pop_back();
            cache_pks_.pop_back();
            cache_index_.erase(it);
          }
        } else {
          if (it != cache_index_.end()) {
            size_t oldb = row_memory_bytes(cache_[it->second]);
            size_t newb = row_memory_bytes(pv.row);
            if (newb >= oldb) cache_bytes_ += newb - oldb;
            else cache_bytes_ -= oldb - newb;
            cache_[it->second] = pv.row;
          } else {
            cache_bytes_ += row_memory_bytes(pv.row);
            cache_index_[pk] = static_cast<uint32_t>(cache_.size());
            cache_pks_.push_back(pk);
            cache_.push_back(pv.row);
          }
        }
      }
    }
    if (cache_valid_ && cache_bytes_ > cache_limit_) {
      cache_.clear();
      cache_pks_.clear();
      cache_index_.clear();
      cache_valid_ = false;
      cache_bytes_ = 0;
    }
    pending_.clear();
    pending_latest_.clear();
    active_txn_ = 0;
  }

  void rollback_pending() {
    pending_.clear();
    pending_latest_.clear();
    active_txn_ = 0;
  }

  bool visible_exists(int64_t pk) const {
    auto pit = pending_latest_.find(pk);
    if (pit != pending_latest_.end()) {
      return !pending_[pit->second].meta.deleted;
    }
    auto lit = latest_.find(pk);
    if (lit == latest_.end()) return false;
    return !versions_[lit->second].deleted;
  }

  bool has_pending() const { return !pending_.empty(); }

  size_t pending_size() const { return pending_.size(); }

  void rollback_to(size_t mark) {
    if (pending_.size() <= mark) return;
    pending_.resize(mark);
    pending_latest_.clear();
    for (size_t i = 0; i < pending_.size(); ++i) {
      pending_latest_[pending_[i].meta.pk] = static_cast<uint32_t>(i);
    }
  }

  const PendingVersion* pending_for(int64_t pk) const {
    auto it = pending_latest_.find(pk);
    if (it == pending_latest_.end()) return nullptr;
    return &pending_[it->second];
  }

  bool pending_deleted(int64_t pk) const {
    const PendingVersion* pv = pending_for(pk);
    return pv && pv->meta.deleted;
  }

  bool get_visible_row(int64_t pk, Row& out) {
    const PendingVersion* pv = pending_for(pk);
    if (pv) {
      if (pv->meta.deleted) return false;
      out = pv->row;
      return true;
    }
    auto it = latest_.find(pk);
    if (it == latest_.end()) return false;
    const Version& v = versions_[it->second];
    if (v.deleted) return false;
    if (cache_valid_) {
      auto cit = cache_index_.find(pk);
      if (cit != cache_index_.end()) {
        out = cache_[cit->second];
        return true;
      }
    }
    out = read_row(v);
    return true;
  }

  Row read_row(const Version& v) {
    std::string rec(v.len, '\0');
    pread_all(fd_, rec.data(), v.len, v.offset);
    return decode_row(schema_, rec.data() + 28, v.len - 28);
  }

  template <typename F>
  void scan_visible(F&& f) {
    if (pending_.empty() && cache_valid_) {
      for (size_t i = 0; i < cache_.size(); ++i) {
        f(cache_pks_[i], cache_[i]);
      }
      return;
    }
    std::unordered_set<int64_t> seen;
    for (const auto& [pk, idx] : pending_latest_) {
      const PendingVersion& pv = pending_[idx];
      seen.insert(pk);
      if (!pv.meta.deleted) f(pk, pv.row);
    }
    if (cache_valid_) {
      for (size_t i = 0; i < cache_.size(); ++i) {
        int64_t pk = cache_pks_[i];
        if (seen.count(pk)) continue;
        f(pk, cache_[i]);
      }
      return;
    }
    for (const auto& [pk, idx] : latest_) {
      if (seen.count(pk)) continue;
      const Version& v = versions_[idx];
      if (v.deleted) continue;
      Row r = read_row(v);
      f(pk, r);
    }
  }

  void fsync_data() {
    if (::fdatasync(fd_) != 0) {
      throw DbError(1105, std::string("data sync failed: ") + std::strerror(errno));
    }
  }

  const std::unordered_map<int64_t, uint32_t>& latest() const { return latest_; }
  const std::vector<Version>& versions() const { return versions_; }

 private:
  std::string dir_;
  TableSchema schema_;
  std::string path_;
  int fd_ = -1;
  uint64_t valid_end_ = 0;
  size_t cache_limit_ = 64ull * 1024 * 1024;
  bool cache_valid_ = false;
  size_t cache_bytes_ = 0;
  std::vector<Row> cache_;
  std::vector<int64_t> cache_pks_;
  std::unordered_map<int64_t, uint32_t> cache_index_;

  std::vector<Version> versions_;
  std::unordered_map<int64_t, uint32_t> latest_;

  uint64_t active_txn_ = 0;
  std::vector<PendingVersion> pending_;
  std::unordered_map<int64_t, uint32_t> pending_latest_;

  void build_cache() {
    cache_.clear();
    cache_pks_.clear();
    cache_index_.clear();
    cache_valid_ = false;
    size_t bytes = 0;
    for (const auto& [pk, idx] : latest_) {
      const Version& v = versions_[idx];
      if (v.deleted) continue;
      Row r = read_row(v);
      bytes += row_memory_bytes(r);
      if (bytes > cache_limit_) {
        cache_.clear();
        cache_pks_.clear();
        cache_index_.clear();
        cache_bytes_ = 0;
        return;
      }
      cache_index_[pk] = static_cast<uint32_t>(cache_.size());
      cache_pks_.push_back(pk);
      cache_.push_back(std::move(r));
    }
    cache_bytes_ = bytes;
    cache_valid_ = true;
  }
};

}  // namespace mvdb
