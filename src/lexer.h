#pragma once

#include "common.h"

namespace mvdb {

enum class Tok {
  End,
  Ident,
  Number,
  String,
  LParen,
  RParen,
  Comma,
  Semicolon,
  Star,
  Dot,
  Eq,
  Ne,
  Lt,
  Le,
  Gt,
  Ge,
  LBracket,
  RBracket,
  Plus,
  Minus
};

struct Token {
  Tok kind = Tok::End;
  std::string text;
  bool quoted = false;
  size_t pos = 0;
};

inline const char* tok_name(Tok t) {
  switch (t) {
    case Tok::End: return "END";
    case Tok::Ident: return "IDENT";
    case Tok::Number: return "NUMBER";
    case Tok::String: return "STRING";
    case Tok::LParen: return "(";
    case Tok::RParen: return ")";
    case Tok::Comma: return ",";
    case Tok::Semicolon: return ";";
    case Tok::Star: return "*";
    case Tok::Dot: return ".";
    case Tok::Eq: return "=";
    case Tok::Ne: return "!=";
    case Tok::Lt: return "<";
    case Tok::Le: return "<=";
    case Tok::Gt: return ">";
    case Tok::Ge: return ">=";
    case Tok::LBracket: return "[";
    case Tok::RBracket: return "]";
    case Tok::Plus: return "+";
    case Tok::Minus: return "-";
  }
  return "?";
}

inline bool ident_start(unsigned char c) {
  return std::isalpha(c) || c == '_' || c == '$' || c == '@' || c >= 0x80;
}

inline bool ident_char(unsigned char c) {
  return std::isalnum(c) || c == '_' || c == '$' || c == '@' || c >= 0x80;
}

class Lexer {
 public:
  explicit Lexer(std::string input) : s_(std::move(input)) { scan(); }

  const std::vector<Token>& tokens() const { return t_; }

 private:
  std::string s_;
  size_t i_ = 0;
  std::vector<Token> t_;

  void push(Tok k, std::string text = {}, bool quoted = false) {
    t_.push_back(Token{k, std::move(text), quoted, i_});
  }

  void scan_string(char quote) {
    ++i_;
    std::string out;
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == quote) {
        if (i_ < s_.size() && s_[i_] == quote) {
          out.push_back(quote);
          ++i_;
          continue;
        }
        push(Tok::String, std::move(out));
        return;
      }
      if (c == '\\' && i_ < s_.size()) {
        char e = s_[i_++];
        switch (e) {
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case '0': out.push_back('\0'); break;
          default: out.push_back(e); break;
        }
        continue;
      }
      out.push_back(c);
    }
    throw DbError(1064, "Unterminated string literal", "42000");
  }

  void scan_backtick() {
    ++i_;
    std::string out;
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '`') {
        if (i_ < s_.size() && s_[i_] == '`') {
          out.push_back('`');
          ++i_;
          continue;
        }
        push(Tok::Ident, std::move(out), true);
        return;
      }
      out.push_back(c);
    }
    throw DbError(1064, "Unterminated quoted identifier", "42000");
  }

  void scan_number() {
    size_t start = i_;
    while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    if (i_ < s_.size() && s_[i_] == '.') {
      ++i_;
      while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      size_t save = i_;
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      if (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) {
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
      } else {
        i_ = save;
      }
    }
    push(Tok::Number, s_.substr(start, i_ - start));
  }

  void scan_ident() {
    size_t start = i_;
    while (i_ < s_.size() && ident_char(static_cast<unsigned char>(s_[i_]))) ++i_;
    push(Tok::Ident, s_.substr(start, i_ - start));
  }

  void scan() {
    while (i_ < s_.size()) {
      unsigned char c = static_cast<unsigned char>(s_[i_]);
      if (std::isspace(c)) {
        ++i_;
        continue;
      }
      if (c == '-' && i_ + 1 < s_.size() && s_[i_ + 1] == '-') {
        i_ += 2;
        while (i_ < s_.size() && s_[i_] != '\n') ++i_;
        continue;
      }
      if (c == '#') {
        while (i_ < s_.size() && s_[i_] != '\n') ++i_;
        continue;
      }
      if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '*') {
        i_ += 2;
        while (i_ + 1 < s_.size() && !(s_[i_] == '*' && s_[i_ + 1] == '/')) ++i_;
        if (i_ + 1 < s_.size()) i_ += 2;
        continue;
      }
      switch (c) {
        case '(': push(Tok::LParen); ++i_; continue;
        case ')': push(Tok::RParen); ++i_; continue;
        case ',': push(Tok::Comma); ++i_; continue;
        case ';': push(Tok::Semicolon); ++i_; continue;
        case '*': push(Tok::Star); ++i_; continue;
        case '.': push(Tok::Dot); ++i_; continue;
        case '[': push(Tok::LBracket); ++i_; continue;
        case ']': push(Tok::RBracket); ++i_; continue;
        case '+': push(Tok::Plus); ++i_; continue;
        case '-': push(Tok::Minus); ++i_; continue;
        case '=': push(Tok::Eq); ++i_; continue;
        case '<':
          if (i_ + 1 < s_.size() && s_[i_ + 1] == '=') {
            i_ += 2;
            push(Tok::Le);
          } else if (i_ + 1 < s_.size() && s_[i_ + 1] == '>') {
            i_ += 2;
            push(Tok::Ne);
          } else {
            ++i_;
            push(Tok::Lt);
          }
          continue;
        case '>':
          if (i_ + 1 < s_.size() && s_[i_ + 1] == '=') {
            i_ += 2;
            push(Tok::Ge);
          } else {
            ++i_;
            push(Tok::Gt);
          }
          continue;
        case '!':
          if (i_ + 1 < s_.size() && s_[i_ + 1] == '=') {
            i_ += 2;
            push(Tok::Ne);
            continue;
          }
          throw DbError(1064, "Syntax error near '!'", "42000");
        case '\'':
        case '"':
          scan_string(static_cast<char>(c));
          continue;
        case '`':
          scan_backtick();
          continue;
        default:
          break;
      }
      if (std::isdigit(c) || (c == '.' && i_ + 1 < s_.size() &&
                              std::isdigit(static_cast<unsigned char>(s_[i_ + 1])))) {
        scan_number();
        continue;
      }
      if (ident_start(c)) {
        scan_ident();
        continue;
      }
      throw DbError(1064, std::string("Syntax error near '") +
                              std::string(1, static_cast<char>(c)) + "'",
                    "42000");
    }
    t_.push_back(Token{Tok::End, {}, false, i_});
  }
};

// Split a text stream into SQL statements at top-level semicolons. Semicolons
// inside strings, quoted identifiers and comments do not split.
inline std::vector<std::string> split_statements(const std::string& input) {
  std::vector<std::string> out;
  std::string cur;
  enum class S { Normal, Single, Double, Backtick, LineComment, BlockComment };
  S st = S::Normal;

  auto flush = [&]() {
    std::string t = trim_copy(cur);
    if (!t.empty()) out.push_back(std::move(t));
    cur.clear();
  };

  for (size_t i = 0; i < input.size(); ++i) {
    char c = input[i];
    char n = (i + 1 < input.size()) ? input[i + 1] : '\0';
    switch (st) {
      case S::Normal:
        if (c == '\'') { st = S::Single; cur.push_back(c); }
        else if (c == '"') { st = S::Double; cur.push_back(c); }
        else if (c == '`') { st = S::Backtick; cur.push_back(c); }
        else if (c == '-' && n == '-') { st = S::LineComment; cur.push_back(c); cur.push_back(n); ++i; }
        else if (c == '#') { st = S::LineComment; cur.push_back(c); }
        else if (c == '/' && n == '*') { st = S::BlockComment; cur.push_back(c); cur.push_back(n); ++i; }
        else if (c == ';') { flush(); }
        else { cur.push_back(c); }
        break;
      case S::Single:
        cur.push_back(c);
        if (c == '\\' && i + 1 < input.size()) { cur.push_back(input[++i]); }
        else if (c == '\'') {
          if (n == '\'') { cur.push_back(n); ++i; }
          else st = S::Normal;
        }
        break;
      case S::Double:
        cur.push_back(c);
        if (c == '\\' && i + 1 < input.size()) { cur.push_back(input[++i]); }
        else if (c == '"') {
          if (n == '"') { cur.push_back(n); ++i; }
          else st = S::Normal;
        }
        break;
      case S::Backtick:
        cur.push_back(c);
        if (c == '`') {
          if (n == '`') { cur.push_back(n); ++i; }
          else st = S::Normal;
        }
        break;
      case S::LineComment:
        cur.push_back(c);
        if (c == '\n') st = S::Normal;
        break;
      case S::BlockComment:
        cur.push_back(c);
        if (c == '*' && n == '/') { cur.push_back(n); ++i; st = S::Normal; }
        break;
    }
  }
  flush();
  return out;
}

}  // namespace mvdb
