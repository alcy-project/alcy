// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "pkg/version.h"

namespace pkg {

// One comparison in a requirement.
enum class VersionOp : u8 {
  Equal,
  Greater,
  GreaterEqual,
  Less,
  LessEqual,
};

struct VersionBound {
  VersionOp op = VersionOp::Equal;
  Version version;

  constexpr bool operator==(const VersionBound&) const = default;
};

// A requirement as a conjunction of bounds. The wildcard terms expand
// to two bounds at parse time, so resolution never sees the spelling.
struct VersionReq {
  static constexpr usize MAX_BOUNDS = 8;
  VersionBound bounds[MAX_BOUNDS];
  u32 count = 0;

  constexpr bool operator==(const VersionReq&) const = default;
};

enum class VersionReqError : u8 {
  Empty,
  BadFormat,
  TooManyBounds,
};

// Parses a requirement (ADR-0057):
//
//   requirement = term ("," term)*
//   term        = comparator X.Y.Z | X.Y.Z | X.Y.x | X.x
//   comparator  = "=" | ">" | ">=" | "<" | "<="
//
// `X.Y.Z` is exact, `X.Y.x` covers [X.Y.0, X.(Y+1).0), and `X.x`
// covers [X.0.0, (X+1).0.0). Bare `x` and `*` are refused; `0.x` is
// not special. Commas are conjunction, whitespace around terms and
// after a comparator is ignored, and prerelease and build metadata
// stay out with parse_version.
base::Result<VersionReq, VersionReqError> parse_version_req(
    std::string_view text);

}  // namespace pkg
