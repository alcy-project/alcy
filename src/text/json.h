// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace text {

// Appends `value` to `out` as a JSON string literal, quotes and
// escaping included. Shared by every writer of JSON so that the copies
// cannot drift apart on which byte needs escaping.
void append_json_string(std::string& out, std::string_view value);

// Appends `value` as a JSON number: decimal digits, no quoting, no
// locale. Going through the formatter for this rather than formatting by
// hand keeps the literal braces out of the surrounding raw strings, where
// a `}` that is not a placeholder is easy to miscount.
template <typename T>
void append_json_number(std::string& out, T value) {
  out += std::to_string(value);
}

// The kind of value a `JsonValue` holds.
enum class JsonKind : u8 {
  Null,
  Bool,
  Number,
  String,
  Array,
  Object,
};

// A JSON value. Objects preserve the order their members were read in and
// look a key up by linear scan; a protocol message carries a handful of
// fields, so a map would cost more than it saves.
class JsonValue {
 public:
  JsonValue() = default;

  static JsonValue make_bool(bool value);
  static JsonValue make_number(f64 value);
  static JsonValue make_string(std::string value);
  static JsonValue make_array();
  static JsonValue make_object();

  JsonKind kind() const { return kind_; }
  bool is_null() const { return kind_ == JsonKind::Null; }
  bool is_bool() const { return kind_ == JsonKind::Bool; }
  bool is_number() const { return kind_ == JsonKind::Number; }
  bool is_string() const { return kind_ == JsonKind::String; }
  bool is_array() const { return kind_ == JsonKind::Array; }
  bool is_object() const { return kind_ == JsonKind::Object; }

  bool as_bool() const { return bool_; }
  f64 as_number() const { return number_; }
  std::string_view as_string() const { return string_; }

  // The members of an array, in order. Empty for every other kind.
  std::span<const JsonValue> as_array() const { return elements_; }

  // The value of a member of an object, or null when `key` is absent or
  // this value is not an object. The pointer is valid until the value is
  // mutated.
  const JsonValue* find(std::string_view key) const;

  // The key of the member at `index` of an object. Empty when out of
  // range.
  std::string_view key_at(usize index) const;

  // Convenience reads that fall back rather than assert, so a message
  // missing an optional field reads as its default.
  bool bool_or(bool fallback) const;
  f64 number_or(f64 fallback) const;
  std::string_view string_or(std::string_view fallback) const;

  // Appends one element to an array. A no-op on any other kind.
  void push(JsonValue value);

  // Adds or replaces the member `key` of an object. A no-op on any other
  // kind.
  void set(std::string key, JsonValue value);

 private:
  JsonKind kind_ = JsonKind::Null;
  bool bool_ = false;
  f64 number_ = 0.0;
  std::string string_;
  // Array elements, and object member values with `keys_` alongside.
  std::vector<JsonValue> elements_;
  std::vector<std::string> keys_;
};

// A parse failure: the byte where parsing stopped, and why.
struct JsonError {
  usize offset = 0;
  std::string message;
};

// Parses one JSON value from `input`. Whitespace may surround it, but any
// other trailing byte is an error, so a frame cannot smuggle a second
// value past the reader. Nesting deeper than `base::MAX_NESTING` is
// rejected rather than recursed into.
base::Result<JsonValue, JsonError> parse_json(std::string_view input);

// Appends `value` as JSON text. The output is a single line with no
// whitespace, since that is what the transport frames.
void append_json(std::string& out, const JsonValue& value);

// `append_json` into a new string.
std::string to_json(const JsonValue& value);

}  // namespace text
