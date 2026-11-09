#pragma once

#include "lexer.h"

#include <memory>
#include <optional>
#include <utility>

namespace mvdb {

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------
enum class ExprKind { Column, Number, String, Vector, L2Distance };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
  ExprKind kind = ExprKind::Number;
  std::string text;   // display text, or column name for Column
  Value value;        // String / Vector literal
  double num = 0.0;   // Number literal
  bool is_integer = false;
  int64_t inum = 0;
  std::vector<ExprPtr> args;  // L2Distance: args[0]=column, args[1]=vector
};

enum class CmpOp { Eq, Ne, Lt, Le, Gt, Ge };

struct Condition {
  std::string column;
  CmpOp op = CmpOp::Eq;
  ExprPtr value;
};

struct SelectItem {
  ExprPtr expr;
  std::string alias;
};

struct OrderItem {
  ExprPtr expr;
  bool asc = true;
};

struct SelectStmt {
  bool star = false;
  bool no_from = false;
  std::vector<SelectItem> items;
  std::string table;
  std::vector<Condition> where;
  std::vector<OrderItem> order;
  bool has_limit = false;
  int64_t limit = -1;
  int64_t offset = 0;
};

struct InsertStmt {
  std::string table;
  std::vector<std::string> columns;
  std::vector<std::vector<ExprPtr>> rows;
};

struct Assignment {
  std::string column;
  ExprPtr value;
};

struct UpdateStmt {
  std::string table;
  std::vector<Assignment> assigns;
  std::vector<Condition> where;
};

struct DeleteStmt {
  std::string table;
  std::vector<Condition> where;
};

struct CreateTableStmt {
  TableSchema schema;
  bool if_not_exists = false;
};

struct CreateIndexStmt {
  std::string name;
  std::string table;
  std::string column;
  bool if_not_exists = false;
};

struct DropTableStmt {
  std::string table;
  bool if_exists = false;
};

struct TxnStmt {
  enum class Kind { Begin, Commit, Rollback } kind = Kind::Begin;
};

struct SetStmt {
  std::string name;
  std::string value;
};

struct ShowStmt {
  enum class Kind { Tables, Describe } kind = Kind::Tables;
  std::string table;
};

using Statement = std::variant<SelectStmt, InsertStmt, UpdateStmt, DeleteStmt,
                               CreateTableStmt, CreateIndexStmt, DropTableStmt,
                               TxnStmt, SetStmt, ShowStmt>;

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
class Parser {
 public:
  explicit Parser(std::string sql) : lx_(std::move(sql)), toks_(lx_.tokens()) {}

  Statement parse() {
    while (accept(Tok::Semicolon)) {
    }
    if (peek().kind == Tok::End) {
      throw DbError(1064, "Empty statement", "42000");
    }

    Statement stmt = parse_statement_inner();
    while (accept(Tok::Semicolon)) {
    }
    if (peek().kind != Tok::End) {
      throw DbError(1064, "Syntax error near '" + peek().text + "'", "42000");
    }
    return stmt;
  }

 private:
  Lexer lx_;
  std::vector<Token> toks_;
  size_t p_ = 0;

  const Token& peek(size_t k = 0) const {
    size_t i = p_ + k;
    if (i >= toks_.size()) return toks_.back();
    return toks_[i];
  }

  bool accept(Tok k) {
    if (peek().kind == k) {
      ++p_;
      return true;
    }
    return false;
  }

  void expect(Tok k, const char* what) {
    if (!accept(k)) {
      throw DbError(1064, std::string("Expected ") + what + " near '" +
                              peek().text + "'",
                    "42000");
    }
  }

  bool kw(const Token& t, const char* k) const {
    return t.kind == Tok::Ident && !t.quoted && upper_copy(t.text) == k;
  }

  bool accept_kw(const char* k) {
    if (kw(peek(), k)) {
      ++p_;
      return true;
    }
    return false;
  }

  void expect_kw(const char* k) {
    if (!accept_kw(k)) {
      throw DbError(1064, std::string("Expected ") + k + " near '" +
                              peek().text + "'",
                    "42000");
    }
  }

  std::string parse_ident() {
    if (peek().kind != Tok::Ident) {
      throw DbError(1064, "Expected identifier near '" + peek().text + "'",
                    "42000");
    }
    std::string s = peek().text;
    ++p_;
    return s;
  }

  static bool is_clause_kw(const Token& t) {
    if (t.kind != Tok::Ident || t.quoted) return false;
    std::string u = upper_copy(t.text);
    return u == "FROM" || u == "WHERE" || u == "ORDER" || u == "LIMIT" ||
           u == "GROUP" || u == "HAVING" || u == "UNION" || u == "JOIN" ||
           u == "ON" || u == "ASC" || u == "DESC";
  }

  int64_t parse_int_literal() {
    const Token& t = peek();
    if (t.kind != Tok::Number) {
      throw DbError(1064, "Expected integer near '" + t.text + "'", "42000");
    }
    if (t.text.find_first_of(".eE") != std::string::npos) {
      throw DbError(1064, "Expected integer near '" + t.text + "'", "42000");
    }
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(t.text.c_str(), &end, 10);
    if (errno == ERANGE || end != t.text.c_str() + t.text.size()) {
      throw DbError(1264, "Integer value out of range", "22003");
    }
    ++p_;
    return static_cast<int64_t>(v);
  }

  ExprPtr make_number(bool negative) {
    const Token& t = peek();
    if (t.kind != Tok::Number) {
      throw DbError(1064, "Expected number near '" + t.text + "'", "42000");
    }
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Number;
    std::string text = (negative ? "-" : "") + t.text;
    e->text = text;
    bool is_int = t.text.find_first_of(".eE") == std::string::npos;
    if (is_int) {
      errno = 0;
      char* end = nullptr;
      long long v = std::strtoll(text.c_str(), &end, 10);
      if (errno == ERANGE || end != text.c_str() + text.size()) {
        throw DbError(1264, "Integer value out of range", "22003");
      }
      e->is_integer = true;
      e->inum = static_cast<int64_t>(v);
      e->num = static_cast<double>(v);
    } else {
      errno = 0;
      char* end = nullptr;
      double d = std::strtod(text.c_str(), &end);
      if (errno == ERANGE || end != text.c_str() + text.size() ||
          !std::isfinite(d)) {
        throw DbError(1264, "Numeric value out of range", "22003");
      }
      e->is_integer = false;
      e->num = d;
    }
    ++p_;
    return e;
  }

  ExprPtr parse_vector_bracket() {
    expect(Tok::LBracket, "'['");
    std::string body;
    while (true) {
      const Token& t = peek();
      if (t.kind == Tok::RBracket) {
        ++p_;
        break;
      }
      if (t.kind == Tok::End) {
        throw DbError(1064, "Unterminated vector literal", "42000");
      }
      if (t.kind == Tok::Number) {
        body += t.text;
        ++p_;
        continue;
      }
      if (t.kind == Tok::Comma) {
        body += ',';
        ++p_;
        continue;
      }
      if (t.kind == Tok::Minus) {
        body += '-';
        ++p_;
        continue;
      }
      if (t.kind == Tok::Plus) {
        body += '+';
        ++p_;
        continue;
      }
      if (t.kind == Tok::Dot) {
        body += '.';
        ++p_;
        continue;
      }
      throw DbError(1064, "Invalid vector literal near '" + t.text + "'",
                    "42000");
    }
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Vector;
    e->value = parse_vector_literal(body, 0);
    e->text = format_value(e->value);
    return e;
  }

  ExprPtr parse_primary() {
    if (accept(Tok::LParen)) {
      ExprPtr e = parse_expr();
      expect(Tok::RParen, "')'");
      return e;
    }
    if (accept(Tok::Minus)) return make_number(true);
    if (accept(Tok::Plus)) return make_number(false);
    if (peek().kind == Tok::Number) return make_number(false);

    if (peek().kind == Tok::String) {
      auto e = std::make_unique<Expr>();
      e->kind = ExprKind::String;
      e->value = peek().text;
      e->text = "'" + peek().text + "'";
      ++p_;
      return e;
    }

    if (peek().kind == Tok::LBracket) return parse_vector_bracket();

    if (peek().kind == Tok::Ident) {
      // VECTOR('[...]') cast form.
      if (kw(peek(), "VECTOR") && peek(1).kind == Tok::LParen) {
        ++p_;
        expect(Tok::LParen, "'('");
        ExprPtr arg = parse_primary();
        expect(Tok::RParen, "')'");
        Vector v;
        if (arg->kind == ExprKind::Vector) {
          v = std::get<Vector>(arg->value);
        } else if (arg->kind == ExprKind::String) {
          v = parse_vector_literal(std::get<std::string>(arg->value), 0);
        } else {
          throw DbError(1064, "VECTOR() requires a vector literal", "42000");
        }
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Vector;
        e->value = std::move(v);
        e->text = format_value(e->value);
        return e;
      }

      std::string name = parse_ident();
      // Optional table qualifier: table.column
      if (accept(Tok::Dot)) {
        name = parse_ident();
      }
      auto e = std::make_unique<Expr>();
      e->kind = ExprKind::Column;
      e->text = name;
      return e;
    }

    throw DbError(1064, "Unexpected token '" + peek().text + "'", "42000");
  }

  ExprPtr parse_l2_distance() {
    ++p_;  // L2_DISTANCE
    expect(Tok::LParen, "'('");
    ExprPtr a = parse_primary();
    expect(Tok::Comma, "','");
    ExprPtr b = parse_primary();
    expect(Tok::RParen, "')'");

    ExprPtr col;
    ExprPtr vec;
    if (a->kind == ExprKind::Column && b->kind != ExprKind::Column) {
      col = std::move(a);
      vec = std::move(b);
    } else if (b->kind == ExprKind::Column && a->kind != ExprKind::Column) {
      col = std::move(b);
      vec = std::move(a);
    } else if (a->kind == ExprKind::Column && b->kind == ExprKind::Column) {
      throw DbError(1064, "L2_DISTANCE requires a vector literal", "42000");
    } else {
      throw DbError(1064, "L2_DISTANCE requires a column argument", "42000");
    }

    if (vec->kind == ExprKind::String) {
      vec->kind = ExprKind::Vector;
      vec->value = parse_vector_literal(std::get<std::string>(vec->value), 0);
      vec->text = format_value(vec->value);
    }
    if (vec->kind != ExprKind::Vector) {
      throw DbError(1064, "L2_DISTANCE requires a vector literal", "42000");
    }

    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::L2Distance;
    e->args.push_back(std::move(col));
    e->args.push_back(std::move(vec));
    e->text = "L2_DISTANCE(" + e->args[0]->text + ", '" +
              format_value(e->args[1]->value) + "')";
    return e;
  }

  ExprPtr parse_expr() {
    if (kw(peek(), "L2_DISTANCE") && peek(1).kind == Tok::LParen) {
      return parse_l2_distance();
    }
    return parse_primary();
  }

  ExprPtr parse_literal_expr() {
    ExprPtr e = parse_primary();
    if (e->kind == ExprKind::Column) {
      throw DbError(1064, "Expected literal, found column '" + e->text + "'",
                    "42000");
    }
    return e;
  }

  CmpOp parse_cmp_op() {
    switch (peek().kind) {
      case Tok::Eq: ++p_; return CmpOp::Eq;
      case Tok::Ne: ++p_; return CmpOp::Ne;
      case Tok::Lt: ++p_; return CmpOp::Lt;
      case Tok::Le: ++p_; return CmpOp::Le;
      case Tok::Gt: ++p_; return CmpOp::Gt;
      case Tok::Ge: ++p_; return CmpOp::Ge;
      default:
        throw DbError(1064, "Expected comparison operator near '" +
                                peek().text + "'",
                      "42000");
    }
  }

  static CmpOp swapped(CmpOp op) {
    switch (op) {
      case CmpOp::Lt: return CmpOp::Gt;
      case CmpOp::Le: return CmpOp::Ge;
      case CmpOp::Gt: return CmpOp::Lt;
      case CmpOp::Ge: return CmpOp::Le;
      default: return op;
    }
  }

  Condition parse_comparison() {
    ExprPtr left = parse_primary();
    CmpOp op = parse_cmp_op();
    ExprPtr right = parse_primary();

    Condition c;
    if (left->kind == ExprKind::Column && right->kind != ExprKind::Column) {
      c.column = left->text;
      c.op = op;
      c.value = std::move(right);
    } else if (right->kind == ExprKind::Column &&
               left->kind != ExprKind::Column) {
      c.column = right->text;
      c.op = swapped(op);
      c.value = std::move(left);
    } else {
      throw DbError(1064, "Comparison requires one column and one literal",
                    "42000");
    }
    if (c.value->kind != ExprKind::Number &&
        c.value->kind != ExprKind::String) {
      throw DbError(1064, "Comparison value must be a number or string",
                    "42000");
    }
    return c;
  }

  void parse_and_list(std::vector<Condition>& out) {
    if (accept(Tok::LParen)) {
      parse_and_list(out);
      expect(Tok::RParen, "')'");
    } else {
      out.push_back(parse_comparison());
    }
    while (accept_kw("AND")) {
      if (accept(Tok::LParen)) {
        parse_and_list(out);
        expect(Tok::RParen, "')'");
      } else {
        out.push_back(parse_comparison());
      }
    }
  }

  std::vector<Condition> parse_where() {
    std::vector<Condition> out;
    parse_and_list(out);
    return out;
  }

  // -------------------------------------------------------------------------
  // Statements
  // -------------------------------------------------------------------------
  Statement parse_create_table(bool if_not_exists) {
    TableSchema s;
    s.name = parse_ident();
    expect(Tok::LParen, "'('");

    std::string table_pk;
    while (true) {
      if (kw(peek(), "PRIMARY")) {
        ++p_;
        expect_kw("KEY");
        expect(Tok::LParen, "'('");
        table_pk = parse_ident();
        expect(Tok::RParen, "')'");
      } else {
        Column c;
        c.name = parse_ident();
        std::string type = upper_copy(parse_ident());
        if (type == "BIGINT" || type == "INT" || type == "INTEGER") {
          c.type = ColType::BigInt;
        } else if (type == "VARCHAR") {
          c.type = ColType::Varchar;
          expect(Tok::LParen, "'('");
          c.len = static_cast<int>(parse_int_literal());
          expect(Tok::RParen, "')'");
        } else if (type == "VECTOR") {
          c.type = ColType::Vector;
          expect(Tok::LParen, "'('");
          c.len = static_cast<int>(parse_int_literal());
          expect(Tok::RParen, "')'");
        } else {
          throw DbError(1064, "Unsupported column type '" + type + "'",
                        "42000");
        }
        while (true) {
          if (accept_kw("PRIMARY")) {
            expect_kw("KEY");
            c.primary = true;
          } else if (accept_kw("NOT")) {
            expect_kw("NULL");
          } else if (accept_kw("UNIQUE")) {
            // accepted and ignored
          } else {
            break;
          }
        }
        s.columns.push_back(std::move(c));
      }
      if (accept(Tok::Comma)) continue;
      break;
    }
    expect(Tok::RParen, "')'");

    for (size_t i = 0; i < s.columns.size(); ++i) {
      for (size_t j = i + 1; j < s.columns.size(); ++j) {
        if (s.columns[i].name == s.columns[j].name) {
          throw DbError(1064, "Duplicate column '" + s.columns[i].name + "'",
                        "42000");
        }
      }
    }
    if (!table_pk.empty()) {
      bool found = false;
      for (auto& c : s.columns) {
        if (c.name == table_pk) {
          c.primary = true;
          found = true;
        }
      }
      if (!found) {
        throw DbError(1064, "Unknown primary key column '" + table_pk + "'",
                      "42000");
      }
    }
    for (size_t i = 0; i < s.columns.size(); ++i) {
      if (s.columns[i].primary) s.pk = static_cast<int>(i);
      if (s.columns[i].type == ColType::Vector) s.vec = static_cast<int>(i);
    }
    validate_schema(s);
    return CreateTableStmt{std::move(s), if_not_exists};
  }

  Statement parse_create_index() {
    CreateIndexStmt st;
    expect_kw("INDEX");
    st.name = parse_ident();
    expect_kw("ON");
    st.table = parse_ident();
    expect(Tok::LParen, "'('");
    st.column = parse_ident();
    expect(Tok::RParen, "')'");
    return st;
  }

  Statement parse_insert() {
    InsertStmt st;
    expect_kw("INTO");
    st.table = parse_ident();
    if (accept(Tok::LParen)) {
      while (true) {
        st.columns.push_back(parse_ident());
        if (accept(Tok::Comma)) continue;
        break;
      }
      expect(Tok::RParen, "')'");
    }
    expect_kw("VALUES");
    while (true) {
      expect(Tok::LParen, "'('");
      std::vector<ExprPtr> row;
      while (true) {
        row.push_back(parse_literal_expr());
        if (accept(Tok::Comma)) continue;
        break;
      }
      expect(Tok::RParen, "')'");
      st.rows.push_back(std::move(row));
      if (accept(Tok::Comma)) continue;
      break;
    }
    return Statement(std::move(st));
  }

  Statement parse_select() {
    SelectStmt st;
    if (peek().kind == Tok::Ident && peek(1).kind == Tok::Dot &&
        peek(2).kind == Tok::Star) {
      p_ += 3;
      st.star = true;
    } else if (accept(Tok::Star)) {
      st.star = true;
    } else {
      while (true) {
        SelectItem it;
        it.expr = parse_expr();
        if (accept_kw("AS")) {
          it.alias = parse_ident();
        } else if (peek().kind == Tok::Ident && !is_clause_kw(peek()) &&
                   peek().kind != Tok::Comma) {
          it.alias = parse_ident();
        }
        st.items.push_back(std::move(it));
        if (accept(Tok::Comma)) continue;
        break;
      }
    }
    if (accept_kw("FROM")) {
      st.table = parse_ident();
      if (accept_kw("WHERE")) st.where = parse_where();
      if (accept_kw("ORDER")) {
        expect_kw("BY");
        while (true) {
          OrderItem oi;
          oi.expr = parse_expr();
          if (accept_kw("DESC")) {
            oi.asc = false;
          } else if (accept_kw("ASC")) {
            oi.asc = true;
          }
          st.order.push_back(std::move(oi));
          if (accept(Tok::Comma)) continue;
          break;
        }
      }
    } else {
      st.no_from = true;
    }
    if (accept_kw("LIMIT")) {
      st.has_limit = true;
      int64_t first = parse_int_literal();
      if (accept(Tok::Comma)) {
        st.offset = first;
        st.limit = parse_int_literal();
      } else if (accept_kw("OFFSET")) {
        st.limit = first;
        st.offset = parse_int_literal();
      } else {
        st.limit = first;
      }
      if (st.limit < 0 || st.offset < 0) {
        throw DbError(1064, "LIMIT must not be negative", "42000");
      }
    }
    return Statement(std::move(st));
  }

  Statement parse_update() {
    UpdateStmt st;
    st.table = parse_ident();
    expect_kw("SET");
    while (true) {
      Assignment a;
      a.column = parse_ident();
      expect(Tok::Eq, "'='");
      a.value = parse_literal_expr();
      st.assigns.push_back(std::move(a));
      if (accept(Tok::Comma)) continue;
      break;
    }
    if (accept_kw("WHERE")) st.where = parse_where();
    return Statement(std::move(st));
  }

  Statement parse_delete() {
    DeleteStmt st;
    expect_kw("FROM");
    st.table = parse_ident();
    if (accept_kw("WHERE")) st.where = parse_where();
    return Statement(std::move(st));
  }

  Statement parse_set() {
    SetStmt st;
    std::string name = parse_ident();
    while (!name.empty() && name.front() == '@') name.erase(name.begin());
    st.name = name;
    if (!accept(Tok::Eq)) {
      // allow "SET name value"
    }
    if (peek().kind == Tok::String) {
      st.value = peek().text;
      ++p_;
    } else if (peek().kind == Tok::Number) {
      st.value = peek().text;
      ++p_;
    } else if (peek().kind == Tok::Ident) {
      st.value = peek().text;
      ++p_;
    } else {
      throw DbError(1064, "Expected SET value near '" + peek().text + "'",
                    "42000");
    }
    return st;
  }

  Statement parse_drop() {
    DropTableStmt st;
    if (accept_kw("DATABASE")) {
      if (accept_kw("IF")) {
        expect_kw("EXISTS");
      }
      parse_ident();
      return SetStmt{"database", "drop"};
    }
    expect_kw("TABLE");
    if (accept_kw("IF")) {
      expect_kw("EXISTS");
      st.if_exists = true;
    }
    st.table = parse_ident();
    return st;
  }

  Statement parse_show() {
    if (accept_kw("TABLES")) {
      return ShowStmt{ShowStmt::Kind::Tables, {}};
    }
    ShowStmt st;
    st.kind = ShowStmt::Kind::Describe;
    st.table = parse_ident();
    return st;
  }

  Statement parse_statement_inner() {
    if (accept_kw("CREATE")) {
      if (accept_kw("DATABASE")) {
        if (accept_kw("IF")) {
          expect_kw("NOT");
          expect_kw("EXISTS");
        }
        parse_ident();
        return SetStmt{"database", "create"};
      }
      if (accept_kw("TABLE")) {
        bool if_not_exists = false;
        if (accept_kw("IF")) {
          expect_kw("NOT");
          expect_kw("EXISTS");
          if_not_exists = true;
        }
        return parse_create_table(if_not_exists);
      }
      if (accept_kw("VECTOR")) return parse_create_index();
      throw DbError(1064, "Unsupported CREATE statement", "42000");
    }
    if (accept_kw("INSERT")) return parse_insert();
    if (accept_kw("SELECT")) return parse_select();
    if (accept_kw("UPDATE")) return parse_update();
    if (accept_kw("DELETE")) return parse_delete();
    if (accept_kw("DROP")) return parse_drop();
    if (accept_kw("SHOW")) return parse_show();
    if (accept_kw("DESCRIBE") || accept_kw("DESC")) return parse_show();
    if (accept_kw("BEGIN")) return TxnStmt{TxnStmt::Kind::Begin};
    if (accept_kw("START")) {
      expect_kw("TRANSACTION");
      return TxnStmt{TxnStmt::Kind::Begin};
    }
    if (accept_kw("COMMIT")) {
      accept_kw("WORK");
      return TxnStmt{TxnStmt::Kind::Commit};
    }
    if (accept_kw("ROLLBACK")) {
      accept_kw("WORK");
      return TxnStmt{TxnStmt::Kind::Rollback};
    }
    if (accept_kw("SET")) return parse_set();
    if (accept_kw("USE")) {
      SetStmt st;
      st.name = "use";
      st.value = parse_ident();
      return st;
    }

    throw DbError(1064, "Unsupported statement near '" + peek().text + "'",
                  "42000");
  }
};

}  // namespace mvdb
