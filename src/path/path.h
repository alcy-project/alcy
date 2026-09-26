// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace path {

// File paths stored as file names or package directories canonically use
// '/' on every platform (valid on Windows file APIs too), keeping lockfiles
// and diagnostics portable. Never branch this per platform: mixing native
// separators reintroduces mismatched spellings and invalid TOML escapes.
constexpr char DEFAULT_PATH_SEPARATOR = '/';
// Folded into DEFAULT_PATH_SEPARATOR on Windows; a valid filename character on
// POSIX, so it is only ever treated as a separator under IS_OS_WIN.
constexpr char WINDOWS_PATH_SEPARATOR = '\\';

constexpr std::string_view SOURCE_EXTENSION = ".al";

enum class PathError : u8 { ContainsNul };

// Canonical path value type. The canonical form is established once at
// construction, so joining, comparing, or serializing paths can never
// produce a non-canonical spelling. Backed by a plain std::string: paths
// only flow through build setup, never hot paths.
class Path {
 public:
  // Folds native separators and lexically normalizes (duplicate separators,
  // "." and resolvable ".." segments, trailing slashes). Symlinks are not
  // resolved. Empty input normalizes to ".". Fails only on embedded NUL
  // bytes, which OS APIs cannot represent.
  static base::Result<Path, PathError> from_native(std::string_view path);

  // Appends `child` (which may itself hold separators) and normalizes.
  // An absolute-looking child does not reset the base: it is appended like
  // any other spelling.
  Path join(std::string_view child) const;

  // Path without its last segment. The parent of a bare name is ".";
  // roots and "." are their own parents.
  Path parent() const;

  bool is_absolute() const;

  std::string_view as_view() const { return value_; }
  const char* c_str() const { return value_.c_str(); }

  bool operator==(const Path& other) const { return value_ == other.value_; }
  bool operator==(std::string_view other) const { return as_view() == other; }
  bool operator!=(const Path& other) const { return !(*this == other); }
  bool operator!=(std::string_view other) const { return !(*this == other); }
  bool operator<(const Path& other) const { return value_ < other.value_; }

 private:
  explicit Path(std::string value) noexcept : value_(std::move(value)) {}

  std::string value_;
};

bool operator==(std::string_view lhs, const Path& rhs);

}  // namespace path
