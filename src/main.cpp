#include "engine.h"
#include "json.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

using namespace mvdb;
using nlohmann::json;

namespace {

std::string semantic_code(const DbError& e) {
  switch (e.code) {
    case 1062: return "duplicate_key";
    case 1050: return "table_exists";
    case 1146: return "table_not_found";
    case 1054: return "column_not_found";
    case 1060: return "duplicate_column";
    case 1110: return "column_specified_twice";
    case 1136: return "column_count_mismatch";
    case 1406: return "value_too_long";
    case 1264: return "integer_out_of_range";
    case 1366:
      if (e.message.find("dimension") != std::string::npos) {
        return "dimension_mismatch";
      }
      if (e.message.find("finite") != std::string::npos ||
          e.message.find("vector") != std::string::npos ||
          e.message.find("Vector") != std::string::npos) {
        return "invalid_vector";
      }
      return "invalid_value";
    case 1568: return "transaction_conflict";
    case 1061: return "duplicate_index";
    case 1364: return "missing_value";
    case 1064:
      if (e.message.rfind("Unsupported", 0) == 0) return "unsupported_sql";
      return "syntax_error";
    default:
      return "error_" + std::to_string(e.code);
  }
}

json to_json_value(const Value& v) {
  if (const auto* p = std::get_if<int64_t>(&v)) return json(*p);
  if (const auto* p = std::get_if<double>(&v)) {
    if (!std::isfinite(*p)) return nullptr;
    return json(*p);
  }
  if (const auto* p = std::get_if<std::string>(&v)) return json(*p);
  if (const auto* p = std::get_if<Vector>(&v)) {
    json a = json::array();
    for (float f : *p) {
      if (!std::isfinite(f)) a.push_back(nullptr);
      else a.push_back(f);
    }
    return a;
  }
  return nullptr;
}

json response_for(const Result& r, const json& id) {
  json resp;
  resp["id"] = id;
  resp["ok"] = true;
  if (r.kind == ResultKind::Query) {
    resp["columns"] = r.columns;
    json rows = json::array();
    for (const auto& row : r.rows) {
      json jr = json::array();
      for (const auto& v : row) jr.push_back(to_json_value(v));
      rows.push_back(std::move(jr));
    }
    resp["rows"] = std::move(rows);
  } else if (r.kind == ResultKind::Modify) {
    resp["affected_rows"] = r.affected;
  }
  return resp;
}

void emit(const json& j) {
  std::cout << j.dump() << '\n';
  std::cout.flush();
}

json error_response(const json& id, const std::string& code,
                    const std::string& message) {
  json err;
  err["code"] = code;
  err["message"] = message;
  json resp;
  resp["id"] = id;
  resp["ok"] = false;
  resp["error"] = err;
  return resp;
}

}  // namespace

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  std::string data_dir = "/data";
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
    else if (a == "--cache-mb") cache_mb = static_cast<size_t>(std::stoull(next("--cache-mb")));
    else if (a == "--no-heap") no_heap = true;
    else if (a == "--help" || a == "-h") {
      std::cerr << "usage: minivectordb [--data DIR] [--cache-mb N] [--no-heap]\n";
      return 0;
    } else {
      std::cerr << "unknown argument: " << a << "\n";
      return 2;
    }
  }
  if (const char* e = std::getenv("MVDB_DATA")) data_dir = e;
  if (const char* e = std::getenv("MVDB_CACHE_MB")) {
    cache_mb = static_cast<size_t>(std::strtoull(e, nullptr, 10));
  }
  if (const char* e = std::getenv("MVDB_NO_HEAP")) {
    no_heap = (std::string(e) != "0");
  }

  try {
    Database db(data_dir, cache_mb);
    if (no_heap) db.set_heap_enabled(false);

    // STDIO_PROTOCOL.md: exactly this object, one line, before any request.
    std::cout << "{\"type\":\"ready\"}\n";
    std::cout.flush();

    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty()) continue;
      json req;
      try {
        req = json::parse(line);
      } catch (const std::exception& e) {
        std::cerr << "invalid JSON request: " << e.what() << "\n";
        continue;
      }
      if (!req.is_object()) {
        std::cerr << "request is not an object\n";
        continue;
      }
      json id = req.contains("id") ? req["id"] : json(nullptr);
      const std::string op = req.value("op", "");

      if (op == "shutdown") {
        db.shutdown();
        json resp;
        resp["id"] = id;
        resp["ok"] = true;
        emit(resp);
        return 0;
      }

      if (op != "execute") {
        emit(error_response(id, "unsupported_op", "unsupported operation"));
        continue;
      }

      const std::string sql = req.value("sql", "");
      try {
        Parser p(sql);
        Statement st = p.parse();
        Result r = db.execute(st);
        emit(response_for(r, id));
      } catch (const DbError& e) {
        emit(error_response(id, semantic_code(e), e.message));
      } catch (const std::exception& e) {
        emit(error_response(id, "internal_error", e.what()));
      }
    }

    db.shutdown();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
