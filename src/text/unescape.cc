// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "text/unescape.h"

#include <string>
#include <string_view>

#include "fpag/base/numeric.h"

namespace text {

namespace {

i32 hex_value(char c) {
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

// Appends one code point as UTF-8. The lexer rejects a value that is
// not a Unicode scalar, so a well-formed run reaches this point; the
// test stays as the last guard, because emitting invalid UTF-8 would
// be worse than dropping the escape for a caller that skipped the
// lexer. The parameter is wide enough that an overlong digit run is
// dropped here rather than wrapping into a different character.
void append_utf8(std::string& out, u64 code) {
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
        u64 code = 0;
        bool any = false;
        while (i < body.size() && body[i] != '}') {
          const i32 digit = hex_value(body[i]);
          if (digit < 0) {
            break;
          }
          // Past the largest scalar there is no point accumulating: a
          // long enough run would wrap the unsigned product back into
          // the valid range. append_utf8 drops what is left.
          if (code <= 0x10FFFF) {
            code = code * 16 + static_cast<u64>(digit);
          }
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

u32 char_literal_value(std::string_view spelling) {
  const std::string bytes = unescape_string(spelling);
  if (bytes.empty()) {
    return 0xFFFD;
  }
  // `char` is signed on the targets we build for: cast through `u8`
  // first so a lead byte such as 0xE6 is not sign-extended into a
  // four-byte scalar before the width decode.
  const auto at = [&](usize i) {
    return static_cast<u32>(static_cast<unsigned char>(bytes[i]));
  };
  u32 scalar = at(0);
  usize width = 1;
  if (scalar >= 0xF0) {
    width = 4;
    scalar &= 0x07;
  } else if (scalar >= 0xE0) {
    width = 3;
    scalar &= 0x0F;
  } else if (scalar >= 0xC0) {
    width = 2;
    scalar &= 0x1F;
  }
  if (bytes.size() < width) {
    return 0xFFFD;
  }
  for (usize i = 1; i < width; ++i) {
    scalar = (scalar << 6) | (at(i) & 0x3F);
  }
  return scalar;
}

}  // namespace text
