// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "text/unescape.h"

#include <string>
#include <string_view>

#include "fpag/base/numeric.h"

namespace text {

namespace {

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Appends one code point as UTF-8. Out-of-range and surrogate values
// are dropped: the lexer already rejected a malformed escape, so this
// only sees a well-formed run.
void append_utf8(std::string& out, u32 code) {
  if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
    return;
  }
  if (code < 0x80) {
    out.push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

}  // namespace

std::string unescape_string(std::string_view spelling) {
  std::string_view body = spelling;
  if (body.size() >= 2) {
    body.remove_prefix(1);
    body.remove_suffix(1);
  }
  std::string out;
  out.reserve(body.size());
  for (usize i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (c != '\\' || i + 1 >= body.size()) {
      out.push_back(c);
      continue;
    }
    const char esc = body[++i];
    switch (esc) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case '\\': out.push_back('\\'); break;
      case '"': out.push_back('"'); break;
      case '0': out.push_back('\0'); break;
      case 'u': {
        // `\u{HEX}`: the lexer validated the shape, so read the digits.
        if (i + 1 >= body.size() || body[i + 1] != '{') {
          out.push_back('u');
          break;
        }
        i += 2;
        u32 code = 0;
        bool any = false;
        while (i < body.size() && body[i] != '}') {
          const int digit = hex_value(body[i]);
          if (digit < 0) {
            break;
          }
          code = code * 16 + static_cast<u32>(digit);
          any = true;
          ++i;
        }
        if (!any) {
          out.append("\\u{}");
          break;
        }
        append_utf8(out, code);
        break;
      }
      default: out.push_back(esc); break;
    }
  }
  return out;
}

}  // namespace text
