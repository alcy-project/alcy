// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

namespace text {

// Decodes the escapes the lexer accepts into the bytes a string
// literal denotes. `spelling` is the source text including the
// surrounding quotes. Shared by every consumer of string literals so
// the three copies cannot drift apart.
std::string unescape_string(std::string_view spelling);

}  // namespace text
