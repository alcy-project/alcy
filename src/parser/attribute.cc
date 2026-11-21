// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parser/attribute.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "diag/span.h"
#include "i18n/messages.h"
#include "lexer/token.h"
#include "parser/diag_code.h"
#include "source/source.h"
#include "text/unescape.h"

namespace parser {

namespace {

constexpr std::string_view TARGET = "target";
constexpr std::string_view TESTING = "testing";
constexpr std::string_view KNOWN_OS_VALUES =
    "linux, darwin, windows, wasi, unknown";
constexpr std::string_view KNOWN_ARCH_VALUES =
    "x86_64, aarch64, riscv64, wasm32, unknown";

BuildFacts facts_for(std::string_view triple, bool freestanding, bool testing) {
  BuildFacts facts;
  facts.freestanding = freestanding;
  facts.testing = testing;
  if (triple.find("windows") != std::string_view::npos ||
      triple.find("mingw") != std::string_view::npos ||
      triple.find("msvc") != std::string_view::npos) {
    facts.os = "windows";
  } else if (triple.find("darwin") != std::string_view::npos) {
    facts.os = "darwin";
  } else if (triple.find("linux") != std::string_view::npos) {
    facts.os = "linux";
  } else if (triple.find("wasi") != std::string_view::npos) {
    facts.os = "wasi";
  }
  if (triple.find("x86_64") != std::string_view::npos) {
    facts.arch = "x86_64";
  } else if (triple.find("aarch64") != std::string_view::npos ||
             triple.find("arm64") != std::string_view::npos) {
    facts.arch = "aarch64";
  } else if (triple.find("riscv64") != std::string_view::npos) {
    facts.arch = "riscv64";
  } else if (triple.find("wasm32") != std::string_view::npos) {
    facts.arch = "wasm32";
  }
  return facts;
}

bool list_contains(std::string_view list, std::string_view word) {
  usize start = 0;
  while (start <= list.size()) {
    const usize end = list.find(", ", start);
    const std::string_view entry = list.substr(
        start,
        end == std::string_view::npos ? std::string_view::npos : end - start);
    if (entry == word) {
      return true;
    }
    if (end == std::string_view::npos) {
      return false;
    }
    start = end + 2;
  }
  return false;
}

// One value the gate reads: a boolean or a string, nothing else. A
// value that came straight from `target.<key>` remembers which one, and
// where it sat, so a literal compared against it is named in the
// diagnostic.
struct Value {
  enum class Kind : u8 { Boolean, String } kind;
  bool boolean = false;
  std::string string;
  std::string_view target_key;
  diag::Span span;

  static Value make_boolean(bool boolean,
                            std::string_view key,
                            diag::Span span) {
    Value value;
    value.kind = Kind::Boolean;
    value.boolean = boolean;
    value.target_key = key;
    value.span = span;
    return value;
  }

  static Value make_string(std::string string,
                           std::string_view key,
                           diag::Span span) {
    Value value;
    value.kind = Kind::String;
    value.string = std::move(string);
    value.target_key = key;
    value.span = span;
    return value;
  }
};

struct Cursor {
  std::span<const lexer::Token> tokens;
  usize at = 0;
};

const lexer::Token& peek(const Cursor& cursor) {
  return cursor
      .tokens[cursor.at < cursor.tokens.size() ? cursor.at
                                               : cursor.tokens.size() - 1];
}

void advance(Cursor& cursor) {
  if (cursor.at < cursor.tokens.size()) {
    ++cursor.at;
  }
}

bool expect(Cursor& cursor, lexer::TokenKind kind) {
  if (peek(cursor).kind != kind) {
    return false;
  }
  advance(cursor);
  return true;
}

std::string spelling_of(const lexer::Token& token, std::string_view bytes) {
  const std::string_view raw =
      bytes.substr(token.span.offset, token.span.length);
  if (token.kind == lexer::TokenKind::String) {
    return text::unescape_string(raw);
  }
  return std::string(raw);
}

void emit_unknown_target(diag::DiagBag& bag,
                         const lexer::Token& token,
                         std::string_view bytes) {
  const u32 index = bag.emit<i18n::Key::ParserWhenUnknownTarget>(
      DiagCode::WhenUnknownTarget, token.span, spelling_of(token, bytes));
  (void)index;
}

void emit_unknown_value(diag::DiagBag& bag,
                        diag::Span span,
                        std::string_view literal,
                        std::string_view key,
                        std::string_view known) {
  const u32 index = bag.emit<i18n::Key::ParserWhenUnknownValue>(
      DiagCode::WhenUnknownValue, span, literal, key, known);
  (void)index;
}

void emit_target_is_boolean(diag::DiagBag& bag, diag::Span span) {
  const u32 index = bag.emit<i18n::Key::ParserWhenTargetIsBoolean>(
      DiagCode::WhenTargetIsBoolean, span);
  (void)index;
}

void emit_target_is_string(diag::DiagBag& bag,
                           diag::Span span,
                           std::string_view key) {
  const u32 index = bag.emit<i18n::Key::ParserWhenTargetIsString>(
      DiagCode::WhenTargetIsString, span, key);
  (void)index;
}

void emit_not_boolean(diag::DiagBag& bag, const lexer::Token& token) {
  const u32 index = bag.emit<i18n::Key::ParserWhenNotBoolean>(
      DiagCode::WhenNotBoolean, token.span);
  (void)index;
}

void emit_unexpected(diag::DiagBag& bag,
                     const lexer::Token& token,
                     std::string_view bytes) {
  const u32 index = bag.emit<i18n::Key::ParserWhenUnexpectedToken>(
      DiagCode::WhenUnexpectedToken, token.span, spelling_of(token, bytes));
  (void)index;
}

struct Evaluator {
  std::span<const lexer::Token> inside;
  std::string_view bytes;
  BuildFacts facts;
  diag::DiagBag& bag;
  bool broken = false;

  template <typename Combine>
  std::optional<Value> fold_bool(std::optional<Value> left,
                                 std::optional<Value> right,
                                 Combine combine,
                                 Cursor& cursor) {
    if (!left.has_value() || !right.has_value()) {
      return std::nullopt;
    }
    const std::optional<bool> lhs = require_bool(*left, cursor);
    const std::optional<bool> rhs = require_bool(*right, cursor);
    if (!lhs.has_value() || !rhs.has_value()) {
      return std::nullopt;
    }
    return Value::make_boolean(combine(*lhs, *rhs), {},
                               diag::Span{source::UNKNOWN_FILE, 0, 0});
  }

  std::optional<bool> require_bool(const Value& value, Cursor& cursor) {
    if (value.kind != Value::Kind::Boolean) {
      emit_not_boolean(bag, token_at(cursor));
      broken = true;
      return std::nullopt;
    }
    return value.boolean;
  }

  // The token the cursor last rested on is the one being diagnosed.
  const lexer::Token& token_at(const Cursor& cursor) const {
    return cursor.at == 0 ? inside.front() : inside[cursor.at - 1];
  }

  std::optional<Value> parse_or(Cursor& cursor) {
    std::optional<Value> value = parse_and(cursor);
    while (peek(cursor).kind == lexer::TokenKind::PipePipe) {
      advance(cursor);
      value = fold_bool(
          value, parse_and(cursor), [](bool a, bool b) { return a || b; },
          cursor);
    }
    return value;
  }

  std::optional<Value> parse_and(Cursor& cursor) {
    std::optional<Value> value = parse_cmp(cursor);
    while (peek(cursor).kind == lexer::TokenKind::AmpAmp) {
      advance(cursor);
      value = fold_bool(
          value, parse_cmp(cursor), [](bool a, bool b) { return a && b; },
          cursor);
    }
    return value;
  }

  // A comparison binds looser than `!`; it compares operands of the
  // same kind, and a string literal standing next to a `target.<key>`
  // must name a value the key knows.
  std::optional<Value> parse_cmp(Cursor& cursor) {
    std::optional<Value> left = parse_unary(cursor);
    if (!left.has_value()) {
      return std::nullopt;
    }
    const lexer::TokenKind kind = peek(cursor).kind;
    if (kind != lexer::TokenKind::EqEq && kind != lexer::TokenKind::BangEq) {
      return left;
    }
    advance(cursor);
    std::optional<Value> right = parse_unary(cursor);
    if (!right.has_value()) {
      return std::nullopt;
    }
    validate_literal(*left, *right);
    validate_literal(*right, *left);
    if (left->kind != right->kind) {
      // A `target.<key>` compared to a literal of the other kind names
      // the kinds rather than "not a boolean": the reader learns which
      // side and which shape.
      const std::string_view left_key = left->target_key;
      const std::string_view right_key = right->target_key;
      if ((left_key == "freestanding" || left_key == TESTING) &&
          right_key.empty()) {
        emit_target_is_boolean(bag, right->span);
      } else if ((right_key == "freestanding" || right_key == TESTING) &&
                 left_key.empty()) {
        emit_target_is_boolean(bag, left->span);
      } else if ((left_key == "os" || left_key == "arch") &&
                 right_key.empty()) {
        emit_target_is_string(bag, right->span, left_key);
      } else if ((right_key == "os" || right_key == "arch") &&
                 left_key.empty()) {
        emit_target_is_string(bag, left->span, right_key);
      } else {
        emit_not_boolean(bag, token_at(cursor));
      }
      broken = true;
      return std::nullopt;
    }
    const bool equal = left->kind == Value::Kind::Boolean
                           ? left->boolean == right->boolean
                           : left->string == right->string;
    return Value::make_boolean(kind == lexer::TokenKind::EqEq ? equal : !equal,
                               {}, diag::Span{source::UNKNOWN_FILE, 0, 0});
  }

  void validate_literal(const Value& target_side, const Value& literal_side) {
    if (target_side.target_key != "os" && target_side.target_key != "arch") {
      return;
    }
    if (literal_side.kind != Value::Kind::String) {
      return;
    }
    const std::string_view known =
        target_side.target_key == "os" ? KNOWN_OS_VALUES : KNOWN_ARCH_VALUES;
    if (list_contains(known, literal_side.string)) {
      return;
    }
    emit_unknown_value(bag, literal_side.span, literal_side.string,
                       target_side.target_key, known);
    broken = true;
  }

  std::optional<Value> parse_unary(Cursor& cursor) {
    if (peek(cursor).kind == lexer::TokenKind::Bang) {
      advance(cursor);
      std::optional<Value> inner = parse_unary(cursor);
      if (!inner.has_value()) {
        return std::nullopt;
      }
      const std::optional<bool> boolean = require_bool(*inner, cursor);
      if (!boolean.has_value()) {
        return std::nullopt;
      }
      return Value::make_boolean(!*boolean, {},
                                 diag::Span{source::UNKNOWN_FILE, 0, 0});
    }
    return parse_primary(cursor);
  }

  std::optional<Value> parse_primary(Cursor& cursor) {
    const lexer::Token token = peek(cursor);
    switch (token.kind) {
      case lexer::TokenKind::LParen: {
        advance(cursor);
        std::optional<Value> inner = parse_or(cursor);
        if (!expect(cursor, lexer::TokenKind::RParen)) {
          emit_unexpected(bag, peek(cursor), bytes);
          broken = true;
          return std::nullopt;
        }
        return inner;
      }
      case lexer::TokenKind::True: {
        advance(cursor);
        return Value::make_boolean(true, {}, token.span);
      }
      case lexer::TokenKind::False: {
        advance(cursor);
        return Value::make_boolean(false, {}, token.span);
      }
      case lexer::TokenKind::String: {
        advance(cursor);
        return Value::make_string(spelling_of(token, bytes), {}, token.span);
      }
      case lexer::TokenKind::Ident: {
        const std::string spelling = spelling_of(token, bytes);
        // `testing` names the invocation mode, not a target property,
        // so it is read bare; every other name must be `target.<key>`.
        if (spelling == TESTING) {
          advance(cursor);
          return Value::make_boolean(facts.testing, TESTING, token.span);
        }
        if (spelling != TARGET) {
          emit_unknown_target(bag, token, bytes);
          broken = true;
          return std::nullopt;
        }
        advance(cursor);
        if (!expect(cursor, lexer::TokenKind::Dot)) {
          emit_unknown_target(bag, token, bytes);
          broken = true;
          return std::nullopt;
        }
        const lexer::Token key = peek(cursor);
        if (key.kind != lexer::TokenKind::Ident) {
          emit_unknown_target(bag, key, bytes);
          broken = true;
          return std::nullopt;
        }
        advance(cursor);
        const std::string name = spelling_of(key, bytes);
        if (name == "os") {
          return Value::make_string(std::string(facts.os), "os", key.span);
        }
        if (name == "arch") {
          return Value::make_string(std::string(facts.arch), "arch", key.span);
        }
        if (name == "freestanding") {
          return Value::make_boolean(facts.freestanding, "freestanding",
                                     key.span);
        }
        emit_unknown_target(bag, key, bytes);
        broken = true;
        return std::nullopt;
      }
      default: break;
    }
    emit_unexpected(bag, token, bytes);
    broken = true;
    return std::nullopt;
  }
};

}  // namespace

BuildFacts build_facts(std::string_view triple,
                       bool freestanding,
                       bool testing) {
  return facts_for(triple, freestanding, testing);
}

GateOutcome evaluate_when(std::span<const lexer::Token> inside,
                          std::string_view bytes,
                          BuildFacts facts,
                          diag::DiagBag& bag) {
  if (inside.empty()) {
    const u32 index = bag.emit<i18n::Key::ParserWhenUnexpectedToken>(
        DiagCode::WhenUnexpectedToken, diag::Span{source::UNKNOWN_FILE, 0, 0},
        std::string_view{")"});
    (void)index;
    return GateOutcome::Drop;
  }
  Cursor cursor{inside, 0};
  Evaluator evaluator{.inside = inside,
                      .bytes = bytes,
                      .facts = facts,
                      .bag = bag,
                      .broken = false};
  std::optional<Value> value = evaluator.parse_or(cursor);
  if (evaluator.broken) {
    // The cause was already reported; trailing tokens past it are
    // cascade, not signal.
    return GateOutcome::Drop;
  }
  if (cursor.at < inside.size()) {
    emit_unexpected(bag, inside[cursor.at], bytes);
    evaluator.broken = true;
  }
  if (!value.has_value() || value->kind != Value::Kind::Boolean) {
    emit_not_boolean(bag, inside.back());
    return GateOutcome::Drop;
  }
  return value->boolean ? GateOutcome::Keep : GateOutcome::Drop;
}

}  // namespace parser
