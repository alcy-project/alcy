// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
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

}  // namespace text
