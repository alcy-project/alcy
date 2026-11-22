// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "fpag/base/numeric.h"

namespace text {

// Decodes the escapes the lexer accepts into the bytes a string
// literal denotes. `spelling` is the source text including the
// surrounding quotes. Shared by every consumer of string literals so
// the three copies cannot drift apart.
std::string unescape_string(std::string_view spelling);

// The Unicode scalar value a character literal denotes, including its
// surrounding quotes. The lexer checked the spelling, so the result is
// always a scalar; callers still guard against an empty decode.
u32 char_literal_value(std::string_view spelling);

}  // namespace text
