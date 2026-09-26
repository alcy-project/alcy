// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "path/path.h"

#include <string>
#include <string_view>
#include <vector>

#include "config/build_config.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace path {

namespace {

// Input is already separator-folded here.
std::string normalize_canonical(std::string_view path) {
  const bool absolute = !path.empty() && path.front() == DEFAULT_PATH_SEPARATOR;
  std::string out;
  std::vector<usize> starts;
  usize i = absolute ? 1 : 0;
  while (i <= path.size()) {
    usize end = i;
    while (end < path.size() && path[end] != DEFAULT_PATH_SEPARATOR) {
      ++end;
    }
    const std::string_view part = path.substr(i, end - i);
    if (part.empty() || part == ".") {
      // Skip.
    } else if (part == "..") {
      if (!starts.empty()) {
        out.resize(starts.back());
        starts.pop_back();
      } else if (!absolute) {
        if (!out.empty()) {
          out.push_back(DEFAULT_PATH_SEPARATOR);
        }
        out.append("..");
      }
    } else {
      starts.push_back(static_cast<usize>(out.size()));
      if (!out.empty()) {
        out.push_back(DEFAULT_PATH_SEPARATOR);
      }
      out.append(part);
    }
    i = end + 1;
  }
  if (absolute) {
    return std::string(1, DEFAULT_PATH_SEPARATOR) + out;
  }
  return out.empty() ? "." : out;
}

#if BUILD_FLAG(IS_OS_WIN)
std::string fold_separators(std::string_view path) {
  std::string out(path);
  for (char& c : out) {
    if (c == WINDOWS_PATH_SEPARATOR) {
      c = DEFAULT_PATH_SEPARATOR;
    }
  }
  return out;
}
#endif

}  // namespace

base::Result<Path, PathError> Path::from_native(std::string_view path) {
  if (path.find('\0') != std::string_view::npos) {
    return base::make_err(PathError::ContainsNul);
  }
#if BUILD_FLAG(IS_OS_WIN)
  return base::make_ok(Path(normalize_canonical(fold_separators(path))));
#else
  return base::make_ok(Path(normalize_canonical(path)));
#endif
}

Path Path::join(std::string_view child) const {
  std::string out = value_;
  out.push_back(DEFAULT_PATH_SEPARATOR);
#if BUILD_FLAG(IS_OS_WIN)
  for (const char c : child) {
    out.push_back(c == WINDOWS_PATH_SEPARATOR ? DEFAULT_PATH_SEPARATOR : c);
  }
#else
  out.append(child);
#endif
  return Path(normalize_canonical(out));
}

Path Path::parent() const {
  const usize slash = value_.find_last_of(DEFAULT_PATH_SEPARATOR);
  if (slash == std::string::npos) {
    return Path(".");
  }
  if (slash == 0) {
    return Path(std::string(1, DEFAULT_PATH_SEPARATOR));
  }
  return Path(value_.substr(0, slash));
}

bool Path::is_absolute() const {
  return !value_.empty() && value_.front() == DEFAULT_PATH_SEPARATOR;
}

bool operator==(std::string_view lhs, const Path& rhs) {
  return lhs == rhs.as_view();
}

}  // namespace path
