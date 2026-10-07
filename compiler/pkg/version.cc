// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/version.h"

#include <charconv>
#include <system_error>

namespace pkg {

base::Result<Version, VersionError> parse_version(std::string_view text) {
  if (text.empty()) {
    return base::make_err(VersionError::Empty);
  }
  Version version;
  u32* parts[3] = {&version.major, &version.minor, &version.patch};
  for (i32 i = 0; i < 3; ++i) {
    std::string_view part;
    if (i < 2) {
      const usize dot = text.find('.');
      if (dot == std::string_view::npos) {
        return base::make_err(VersionError::BadFormat);
      }
      part = text.substr(0, dot);
      text.remove_prefix(dot + 1);
    } else {
      part = text;
      text = {};
    }
    if (part.empty()) {
      return base::make_err(VersionError::BadFormat);
    }
    u32 value = 0;
    const char* const begin = part.data();
    const char* const end = begin + part.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end) {
      return base::make_err(VersionError::BadFormat);
    }
    *parts[i] = value;
  }
  return base::make_ok(version);
}

}  // namespace pkg
