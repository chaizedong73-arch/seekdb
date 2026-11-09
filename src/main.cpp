#include "engine.h"

#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace mvdb;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

// Serializes all stdout writes on a dedicated thread. The SQL loop can keep
// consuming stdin even if the test driver is slow to drain stdout, which avoids
// a classic pipe deadlock when the driver writes a whole batch before reading.
class Output {
 public:
  Output() { thread_ = std::thread([this] { run(); }); }
  ~Output() { finish(); }

  void enqueue(std::string s) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      queue_.push_back(std::move(s));
    }
    cv_.notify_one();
  }

  void finish() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      done_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;
  bool done_ = false;
  std::thread thread_;

  void run() {
    for (;;) {
      std::string s;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return done_ || !queue_.empty(); });
        if (queue_.empty()) {
          if (done_) return;
          continue;
        }
        s = std::move(queue_.front());
        queue_.pop_front();
      }
      std::cout.write(s.data(), static_cast<std::streamsize>(s.size()));
      std::cout.flush();
    }
  }
};

// Streaming statement splitter: emits a statement whenever a top-level ';' is
// seen. Semicolons inside strings, quoted identifiers and comments do not
// split.
class StatementReader {
 public:
  void feed(const std::string& chunk, std::vector<std::string>& out) {
    for (size_t i = 0; i < chunk.size(); ++i) {
      char c = chunk[i];
      char n = (i + 1 < chunk.size()) ? chunk[i + 1] : '\0';
      switch (st_) {
        case S::Normal:
          if (c == '\'') { st_ = S::Single; buf_.push_back(c); }
          else if (c == '"') { st_ = S::Double; buf_.push_back(c); }
          else if (c == '`') { st_ = S::Backtick; buf_.push_back(c); }
          else if (c == '-' && n == '-') { st_ = S::LineComment; buf_.push_back(c); buf_.push_back(n); ++i; }
          else if (c == '#') { st_ = S::LineComment; buf_.push_back(c); }
          else if (c == '/' && n == '*') { st_ = S::BlockComment; buf_.push_back(c); buf_.push_back(n); ++i; }
          else if (c == ';') { emit(out); }
          else { buf_.push_back(c); }
          break;
        case S::Single:
          buf_.push_back(c);
          if (c == '\\' && i + 1 < chunk.size()) { buf_.push_back(chunk[++i]); }
          else if (c == '\'') {
            if (n == '\'') { buf_.push_back(n); ++i; }
            else st_ = S::Normal;
          }
          break;
        case S::Double:
          buf_.push_back(c);
          if (c == '\\' && i + 1 < chunk.size()) { buf_.push_back(chunk[++i]); }
          else if (c == '"') {
            if (n == '"') { buf_.push_back(n); ++i; }
            else st_ = S::Normal;
          }
          break;
        case S::Backtick:
          buf_.push_back(c);
          if (c == '`') {
            if (n == '`') { buf_.push_back(n); ++i; }
            else st_ = S::Normal;
          }
          break;
        case S::LineComment:
          buf_.push_back(c);
          if (c == '\n') st_ = S::Normal;
          break;
        case S::BlockComment:
          buf_.push_back(c);
          if (c == '*' && n == '/') { buf_.push_back(n); ++i; st_ = S::Normal; }
          break;
      }
    }
  }

  void finish(std::vector<std::string>& out) { emit(out); }

  std::string pending_trimmed() const { return trim_copy(buf_); }

 private:
  enum class S { Normal, Single, Double, Backtick, LineComment, BlockComment };
  S st_ = S::Normal;
  std::string buf_;

  void emit(std::vector<std::string>& out) {
    std::string t = trim_copy(buf_);
    if (!t.empty()) out.push_back(std::move(t));
    buf_.clear();
  }
};

bool is_quit_statement(const std::string& s) {
  std::string t = trim_copy(s);
  if (t == "\\q") return true;
  std::string u = upper_copy(t);
  return u == "EXIT" || u == "QUIT" || u == "\\QUIT";
}

std::string escape_tsv(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '\\': o += "\\\\"; break;
      case '\t': o += "\\t"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\0': o += "\\0"; break;
      default: o += c; break;
    }
  }
  return o;
}

std::string format_error(const DbError& e) {
  return "ERROR " + std::to_string(e.code) + " (" + e.state + "): " +
         e.message + "\n\n";
}

std::string format_result(const Result& r, const std::string& format) {
  if (!r.is_query) {
    return "Query OK, " + std::to_string(r.affected) + " row" +
           (r.affected == 1 ? "" : "s") + " affected\n\n";
  }

  if (format == "mysql") {
    std::vector<size_t> widths(r.columns.size(), 0);
    for (size_t c = 0; c < r.columns.size(); ++c) {
      widths[c] = r.columns[c].size();
      for (const auto& row : r.rows) {
        if (c < row.size()) widths[c] = std::max(widths[c], row[c].size());
      }
    }
    std::ostringstream o;
    auto line = [&]() {
      o << '+';
      for (size_t w : widths) o << std::string(w + 2, '-') << '+';
      o << '\n';
    };
    line();
    o << '|';
    for (size_t c = 0; c < r.columns.size(); ++c) {
      o << ' ' << r.columns[c]
        << std::string(widths[c] - r.columns[c].size(), ' ') << " |";
    }
    o << '\n';
    line();
    for (const auto& row : r.rows) {
      o << '|';
      for (size_t c = 0; c < r.columns.size(); ++c) {
        std::string v = c < row.size() ? row[c] : "";
        o << ' ' << v << std::string(widths[c] - v.size(), ' ') << " |";
      }
      o << '\n';
    }
    line();
    o << r.rows.size() << " row" << (r.rows.size() == 1 ? "" : "s")
      << " in set\n\n";
    return o.str();
  }

  std::string o;
  for (size_t c = 0; c < r.columns.size(); ++c) {
    if (c) o += '\t';
    o += escape_tsv(r.columns[c]);
  }
  o += '\n';
  for (const auto& row : r.rows) {
    for (size_t c = 0; c < row.size(); ++c) {
      if (c) o += '\t';
      o += escape_tsv(row[c]);
    }
    o += '\n';
  }
  o += '\n';
  return o;
}

void execute_one(Database& db, const std::string& sql, const std::string& format,
                 Output& out) {
  try {
    Parser p(sql);
    Statement st = p.parse();
    Result r = db.execute(st);
    out.enqueue(format_result(r, format));
  } catch (const DbError& e) {
    out.enqueue(format_error(e));
  } catch (const std::bad_variant_access&) {
    out.enqueue(format_error(DbError(1105, "internal type error")));
  } catch (const std::exception& e) {
    out.enqueue(format_error(DbError(1105, e.what())));
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  std::string data_dir = "/data";
  std::string ready = "READY";
  std::string mode = "exact";
  std::string format = "tsv";
  size_t cache_mb = 64;
  bool no_heap = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--data") data_dir = next("--data");
    else if (a == "--ready") ready = next("--ready");
    else if (a == "--mode") mode = next("--mode");
    else if (a == "--format") format = next("--format");
    else if (a == "--cache-mb") cache_mb = static_cast<size_t>(std::stoull(next("--cache-mb")));
    else if (a == "--no-heap") no_heap = true;
    else if (a == "--help" || a == "-h") {
      std::cerr << "usage: minivectordb [--data DIR] [--ready TEXT] "
                   "[--mode exact|ann] [--format tsv|mysql] [--cache-mb N] "
                   "[--no-heap]\n";
      return 0;
    } else {
      std::cerr << "unknown argument: " << a << "\n";
      return 2;
    }
  }
  if (const char* e = std::getenv("MVDB_DATA")) data_dir = e;
  if (const char* e = std::getenv("MVDB_READY")) ready = e;
  if (const char* e = std::getenv("MVDB_MODE")) mode = e;
  if (const char* e = std::getenv("MVDB_FORMAT")) format = e;
  if (const char* e = std::getenv("MVDB_CACHE_MB")) {
    cache_mb = static_cast<size_t>(std::strtoull(e, nullptr, 10));
  }
  if (const char* e = std::getenv("MVDB_NO_HEAP")) {
    no_heap = (std::string(e) != "0");
  }

  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // no SA_RESTART: interrupt a blocking getline
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  Output out;
  try {
    Database db(data_dir, cache_mb);
    if (no_heap) db.set_heap_enabled(false);
    if (mode == "ann") {
      SetStmt s;
      s.name = "vector_mode";
      s.value = "ann";
      db.execute(s);
    }

    out.enqueue(ready + "\n");

    StatementReader reader;
    std::vector<std::string> stmts;
    struct pollfd pfd {};
    pfd.fd = 0;
    pfd.events = POLLIN;
    while (!g_stop) {
      int pr = ::poll(&pfd, 1, 200);
      if (pr < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (pr == 0) continue;
      char buf[65536];
      ssize_t n = ::read(0, buf, sizeof(buf));
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (n == 0) break;
      reader.feed(std::string(buf, static_cast<size_t>(n)), stmts);
      for (auto& s : stmts) {
        if (is_quit_statement(s)) {
          g_stop = 1;
          break;
        }
        execute_one(db, s, format, out);
      }
      stmts.clear();
      if (!g_stop && is_quit_statement(reader.pending_trimmed())) {
        g_stop = 1;
      }
    }

    if (!g_stop) {
      reader.finish(stmts);
      for (auto& s : stmts) {
        if (is_quit_statement(s)) break;
        execute_one(db, s, format, out);
      }
    }

    db.shutdown();
    out.finish();
  } catch (const DbError& e) {
    out.enqueue(format_error(e));
    out.finish();
    return 1;
  } catch (const std::exception& e) {
    out.enqueue(format_error(DbError(1105, e.what())));
    out.finish();
    return 1;
  }
  return 0;
}
