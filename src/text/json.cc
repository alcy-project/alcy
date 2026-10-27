// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "text/json.h"

#include <charconv>
#include <cmath>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "base/nesting.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace text {

namespace {

bool parse_float(std::string_view literal, f64* out) {
  const char* begin = literal.data();
  const char* end = begin + literal.size();
  const std::from_chars_result result =
      std::from_chars(begin, end, *out, std::chars_format::general);
  return result.ec == std::errc() && result.ptr == end;
}

// Recursive-descent reader over one input. `error_` latches the first
// failure: a reader reports false, the parser unwinds, and the error says
// where and why for the result.
class JsonReader {
 public:
  explicit JsonReader(std::string_view input) : input_(input) {}

  base::Result<JsonValue, JsonError> parse() {
    skip_whitespace();
    JsonValue value;
    if (!read_value(0, &value)) {
      return base::make_err(error_);
    }
    skip_whitespace();
    if (pos_ != input_.size()) {
      fail(pos_, "unexpected content after the JSON value");
      return base::make_err(error_);
    }
    return base::make_ok(std::move(value));
  }

 private:
  bool read_value(u32 depth, JsonValue* out) {
    if (depth > base::MAX_NESTING) {
      fail(pos_, "JSON nesting is too deep");
      return false;
    }
    if (pos_ >= input_.size()) {
      fail(pos_, "expected a JSON value");
      return false;
    }
    switch (input_[pos_]) {
      case '{': return read_object(depth, out);
      case '[': return read_array(depth, out);
      case '"': {
        std::string parsed;
        if (!read_string(&parsed)) {
          return false;
        }
        *out = JsonValue::make_string(std::move(parsed));
        return true;
      }
      case 't':
        if (!consume("true")) {
          return false;
        }
        *out = JsonValue::make_bool(true);
        return true;
      case 'f':
        if (!consume("false")) {
          return false;
        }
        *out = JsonValue::make_bool(false);
        return true;
      case 'n':
        if (!consume("null")) {
          return false;
        }
        *out = JsonValue{};
        return true;
      default: return read_number(out);
    }
  }

  bool read_object(u32 depth, JsonValue* out) {
    ++pos_;  // '{'
    JsonValue object = JsonValue::make_object();
    skip_whitespace();
    if (eat('}')) {
      *out = std::move(object);
      return true;
    }
    for (;;) {
      skip_whitespace();
      std::string key;
      if (!read_string(&key)) {
        return false;
      }
      skip_whitespace();
      if (!eat(':')) {
        fail(pos_, "expected ':' after an object key");
        return false;
      }
      skip_whitespace();
      JsonValue member;
      if (!read_value(depth + 1, &member)) {
        return false;
      }
      object.set(std::move(key), std::move(member));
      skip_whitespace();
      if (eat(',')) {
        continue;
      }
      if (eat('}')) {
        *out = std::move(object);
        return true;
      }
      fail(pos_, "expected ',' or '}' in an object");
      return false;
    }
  }

  bool read_array(u32 depth, JsonValue* out) {
    ++pos_;  // '['
    JsonValue array = JsonValue::make_array();
    skip_whitespace();
    if (eat(']')) {
      *out = std::move(array);
      return true;
    }
    for (;;) {
      skip_whitespace();
      JsonValue element;
      if (!read_value(depth + 1, &element)) {
        return false;
      }
      array.push(std::move(element));
      skip_whitespace();
      if (eat(',')) {
        continue;
      }
      if (eat(']')) {
        *out = std::move(array);
        return true;
      }
      fail(pos_, "expected ',' or ']' in an array");
      return false;
    }
  }

  bool read_string(std::string* out) {
    if (!eat('"')) {
      fail(pos_, "expected a string");
      return false;
    }
    out->clear();
    while (pos_ < input_.size()) {
      const char c = input_[pos_];
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c == '\\') {
        ++pos_;
        if (!read_escape(out)) {
          return false;
        }
        continue;
      }
      if (static_cast<u8>(c) < 0x20) {
        fail(pos_, "a control byte is not allowed in a string");
        return false;
      }
      out->push_back(c);
      ++pos_;
    }
    fail(pos_, "unterminated string");
    return false;
  }

  bool read_escape(std::string* out) {
    if (pos_ >= input_.size()) {
      fail(pos_, "unterminated escape");
      return false;
    }
    const char c = input_[pos_++];
    switch (c) {
      case '"': out->push_back('"'); return true;
      case '\\': out->push_back('\\'); return true;
      case '/': out->push_back('/'); return true;
      case 'b': out->push_back('\b'); return true;
      case 'f': out->push_back('\f'); return true;
      case 'n': out->push_back('\n'); return true;
      case 'r': out->push_back('\r'); return true;
      case 't': out->push_back('\t'); return true;
      case 'u': return read_unicode_escape(out);
      default: fail(pos_ - 1, "unknown escape in a string"); return false;
    }
  }

  // One \uXXXX escape. A surrogate pair is combined, and a lone surrogate
  // is encoded as the replacement character so every escape yields valid
  // UTF-8 rather than a fragment.
  bool read_unicode_escape(std::string* out) {
    u32 code = 0;
    if (!read_hex4(&code)) {
      return false;
    }
    if (code >= 0xD800 && code <= 0xDBFF) {
      u32 low = 0;
      if (pos_ + 1 < input_.size() && input_[pos_] == '\\' &&
          input_[pos_ + 1] == 'u') {
        const usize save = pos_;
        pos_ += 2;
        if (!read_hex4(&low)) {
          return false;
        }
        if (low >= 0xDC00 && low <= 0xDFFF) {
          code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          append_utf8(code, out);
          return true;
        }
        pos_ = save;
      }
      append_utf8(0xFFFD, out);
      return true;
    }
    if (code >= 0xDC00 && code <= 0xDFFF) {
      append_utf8(0xFFFD, out);
      return true;
    }
    append_utf8(code, out);
    return true;
  }

  bool read_hex4(u32* out) {
    if (pos_ + 4 > input_.size()) {
      fail(pos_, "a \\u escape needs four hex digits");
      return false;
    }
    u32 value = 0;
    for (u32 i = 0; i < 4; ++i) {
      const char c = input_[pos_ + i];
      u32 digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<u32>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<u32>(c - 'a') + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<u32>(c - 'A') + 10;
      } else {
        fail(pos_ + i, "a \\u escape needs four hex digits");
        return false;
      }
      value = (value << 4) | digit;
    }
    pos_ += 4;
    *out = value;
    return true;
  }

  bool read_number(JsonValue* out) {
    const usize start = pos_;
    eat('-');
    if (pos_ >= input_.size()) {
      fail(start, "expected a number");
      return false;
    }
    if (input_[pos_] == '0') {
      ++pos_;
    } else if (input_[pos_] >= '1' && input_[pos_] <= '9') {
      while (pos_ < input_.size() && input_[pos_] >= '0' &&
             input_[pos_] <= '9') {
        ++pos_;
      }
    } else {
      fail(pos_, "expected a number");
      return false;
    }
    bool integral = true;
    if (pos_ < input_.size() && input_[pos_] == '.') {
      integral = false;
      ++pos_;
      if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
        fail(pos_, "expected a digit after '.'");
        return false;
      }
      while (pos_ < input_.size() && input_[pos_] >= '0' &&
             input_[pos_] <= '9') {
        ++pos_;
      }
    }
    if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
      integral = false;
      ++pos_;
      if (pos_ < input_.size() &&
          (input_[pos_] == '+' || input_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
        fail(pos_, "expected a digit in the exponent");
        return false;
      }
      while (pos_ < input_.size() && input_[pos_] >= '0' &&
             input_[pos_] <= '9') {
        ++pos_;
      }
    }
    const std::string_view literal = input_.substr(start, pos_ - start);
    f64 value = 0.0;
    if (integral) {
      i64 integer = 0;
      const char* begin = literal.data();
      const char* end = begin + literal.size();
      const std::from_chars_result result =
          std::from_chars(begin, end, integer);
      if (result.ec == std::errc() && result.ptr == end) {
        value = static_cast<f64>(integer);
      } else if (!parse_float(literal, &value)) {
        fail(start, "number is out of range");
        return false;
      }
    } else if (!parse_float(literal, &value)) {
      fail(start, "number is out of range");
      return false;
    }
    *out = JsonValue::make_number(value);
    return true;
  }

  void append_utf8(u32 code, std::string* out) const {
    if (code <= 0x7F) {
      out->push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
      out->push_back(static_cast<char>(0xC0 | (code >> 6)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0xFFFF) {
      out->push_back(static_cast<char>(0xE0 | (code >> 12)));
      out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (code >> 18)));
      out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  void skip_whitespace() {
    while (pos_ < input_.size()) {
      const char c = input_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool eat(char c) {
    if (pos_ < input_.size() && input_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool consume(std::string_view literal) {
    if (input_.substr(pos_, literal.size()) != literal) {
      fail(pos_, "invalid literal");
      return false;
    }
    pos_ += literal.size();
    return true;
  }

  void fail(usize offset, std::string message) {
    if (error_.message.empty()) {
      error_ = JsonError{offset, std::move(message)};
    }
  }

  std::string_view input_;
  usize pos_ = 0;
  JsonError error_;
};

void append_number(std::string& out, f64 value) {
  if (!std::isfinite(value)) {
    out += "null";
    return;
  }
  // Integral doubles print without a decimal point so an integer id that
  // was read as a number is written back the way it arrived.
  if (value == std::floor(value) && std::fabs(value) < 9007199254740992.0) {
    out += fmt::format("{}", static_cast<i64>(value));
    return;
  }
  out += fmt::format("{}", value);
}

void append_value(std::string& out, const JsonValue& value) {
  switch (value.kind()) {
    case JsonKind::Null: out += "null"; return;
    case JsonKind::Bool: out += value.as_bool() ? "true" : "false"; return;
    case JsonKind::Number: append_number(out, value.as_number()); return;
    case JsonKind::String: append_json_string(out, value.as_string()); return;
    case JsonKind::Array: {
      out.push_back('[');
      bool first = true;
      for (const JsonValue& element : value.as_array()) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        append_value(out, element);
      }
      out.push_back(']');
      return;
    }
    case JsonKind::Object: {
      out.push_back('{');
      for (usize i = 0; i < value.as_array().size(); ++i) {
        if (i != 0) {
          out.push_back(',');
        }
        append_json_string(out, value.key_at(i));
        out.push_back(':');
        append_value(out, value.as_array()[i]);
      }
      out.push_back('}');
      return;
    }
  }
}

}  // namespace

void append_json_string(std::string& out, std::string_view value) {
  out.push_back('"');
  for (const char c : value) {
    switch (c) {
      case '"':
        out.push_back('\\');
        out.push_back('"');
        break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (static_cast<u8>(c) < 0x20) {
          fmt::format_to(std::back_inserter(out), "\\u{:04x}",
                         static_cast<u32>(static_cast<u8>(c)));
        } else {
          // Bytes pass through as they are: the source they came from is
          // UTF-8, and re-encoding would only risk disagreeing with it.
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

JsonValue JsonValue::make_bool(bool value) {
  JsonValue out;
  out.kind_ = JsonKind::Bool;
  out.bool_ = value;
  return out;
}

JsonValue JsonValue::make_number(f64 value) {
  JsonValue out;
  out.kind_ = JsonKind::Number;
  out.number_ = value;
  return out;
}

JsonValue JsonValue::make_string(std::string value) {
  JsonValue out;
  out.kind_ = JsonKind::String;
  out.string_ = std::move(value);
  return out;
}

JsonValue JsonValue::make_array() {
  JsonValue out;
  out.kind_ = JsonKind::Array;
  return out;
}

JsonValue JsonValue::make_object() {
  JsonValue out;
  out.kind_ = JsonKind::Object;
  return out;
}

const JsonValue* JsonValue::find(std::string_view key) const {
  if (kind_ != JsonKind::Object) {
    return nullptr;
  }
  for (usize i = 0; i < keys_.size(); ++i) {
    if (keys_[i] == key) {
      return &elements_[i];
    }
  }
  return nullptr;
}

std::string_view JsonValue::key_at(usize index) const {
  return index < keys_.size() ? std::string_view(keys_[index])
                              : std::string_view{};
}

bool JsonValue::bool_or(bool fallback) const {
  return kind_ == JsonKind::Bool ? bool_ : fallback;
}

f64 JsonValue::number_or(f64 fallback) const {
  return kind_ == JsonKind::Number ? number_ : fallback;
}

std::string_view JsonValue::string_or(std::string_view fallback) const {
  return kind_ == JsonKind::String ? std::string_view(string_) : fallback;
}

void JsonValue::push(JsonValue value) {
  if (kind_ == JsonKind::Array) {
    elements_.push_back(std::move(value));
  }
}

void JsonValue::set(std::string key, JsonValue value) {
  if (kind_ != JsonKind::Object) {
    return;
  }
  for (usize i = 0; i < keys_.size(); ++i) {
    if (keys_[i] == key) {
      elements_[i] = std::move(value);
      return;
    }
  }
  keys_.push_back(std::move(key));
  elements_.push_back(std::move(value));
}

base::Result<JsonValue, JsonError> parse_json(std::string_view input) {
  JsonReader reader(input);
  return reader.parse();
}

void append_json(std::string& out, const JsonValue& value) {
  append_value(out, value);
}

std::string to_json(const JsonValue& value) {
  std::string out;
  append_value(out, value);
  return out;
}

}  // namespace text
