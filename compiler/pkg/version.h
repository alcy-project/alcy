// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace pkg {

// A strict X.Y.Z version, without prerelease or build metadata (MVP).
struct Version {
  u32 major = 0;
  u32 minor = 0;
  u32 patch = 0;
};

constexpr bool operator==(const Version& lhs, const Version& rhs) {
  return lhs.major == rhs.major && lhs.minor == rhs.minor &&
         lhs.patch == rhs.patch;
}

enum class VersionError : u8 {
  Empty,
  BadFormat,
};

// Parses strict X.Y.Z numerics (no prerelease/build metadata in MVP).
base::Result<Version, VersionError> parse_version(std::string_view text);

}  // namespace pkg
