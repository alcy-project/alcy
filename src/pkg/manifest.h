// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <charconv>
#include <string_view>
#include <system_error>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "source/source.h"

namespace pkg {

// Manifest file name looked up in package directories.
constexpr std::string_view kManifestFileName = "alcy.toml";

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
inline base::Result<Version, VersionError> parse_version(
    std::string_view text) {
  if (text.empty()) {
    return base::make_err(VersionError::Empty);
  }
  Version version;
  u32* parts[3] = {&version.major, &version.minor, &version.patch};
  for (int i = 0; i < 3; ++i) {
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

// A path-only dependency (MVP scope: no registry, no git). Views borrow
// arena storage owned by the caller of parse_manifest().
struct Dependency {
  std::string_view name;
  std::string_view path;
};

struct PackageManifest {
  std::string_view name;
  Version version;
  // Optional edition string; empty when absent.
  std::string_view edition;
  // Arena-owned array, possibly empty.
  const Dependency* dependencies = nullptr;
  u32 dependency_count = 0;
};

// Parses manifest bytes; all strings reference arena copies. `file` backs
// spans for syntax errors (pass source::kUnknownFile when unknown).
diag::Fallible<PackageManifest> parse_manifest(std::string_view bytes,
                                               std::string_view filename,
                                               source::FileId file,
                                               diag::DiagBag& bag,
                                               mem::Arena& arena);

}  // namespace pkg
