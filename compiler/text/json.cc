// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "text/json.h"

#include <iterator>
#include <string>
#include <string_view>

#include "fmt/format.h"
#include "fpag/base/numeric.h"

namespace text {

void append_json_string(std::string& out, std::string_view value) {
  out.push_back('"');
  for (const char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (static_cast<u8>(c) < 0x20) {
          fmt::format_to(std::back_inserter(out), "\\u{:04x}",
                         static_cast<unsigned>(static_cast<u8>(c)));
        } else {
          // Bytes pass through as they are: the source they came from is
          // UTF-8, and re-encoding would only risk disagreeing with it.
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

}  // namespace text
