// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "path/path.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/build_config.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace path {

namespace {

// Input is already separator-folded here.
std::string normalize_canonical(std::string_view path) {
  const bool absolute = !path.empty() && path.front() == DEFAULT_PATH_SEPARATOR;
  // The result never outgrows the input plus its leading separator, and a
  // segment takes at least two bytes: reserving both up front leaves the
  // walk below with no reallocation to make.
  std::string out;
  out.reserve(path.size() + 1);
  if (absolute) {
    out.push_back(DEFAULT_PATH_SEPARATOR);
  }
  const usize prefix = out.size();
  std::vector<usize> starts;
  starts.reserve(path.size() / 2 + 1);
  usize i = absolute ? 1 : 0;
  while (i <= path.size()) {
    usize end = i;
    while (end < path.size() && path[end] != DEFAULT_PATH_SEPARATOR) {
      ++end;
    }
    const std::string_view part = path.substr(i, end - i);
    if (part == "..") {
      if (!starts.empty()) {
        out.resize(starts.back());
        starts.pop_back();
      } else if (!absolute) {
        if (!out.empty()) {
          out.push_back(DEFAULT_PATH_SEPARATOR);
        }
        out.append("..");
      }
    } else if (!part.empty() && part != ".") {
      starts.push_back(static_cast<usize>(out.size()));
      if (out.size() > prefix) {
        out.push_back(DEFAULT_PATH_SEPARATOR);
      }
      out.append(part);
    }
    i = end + 1;
  }
  if (absolute) {
    return out;
  }
  return out.empty() ? "." : out;
}

// A name a join appends as one segment: nothing in it to fold into the
// parent's separators, and nothing the normalization walk would drop.
bool is_plain_name(std::string_view name) {
  if (name.empty() || name == "." || name == "..") {
    return false;
  }
  for (const char c : name) {
    if (c == DEFAULT_PATH_SEPARATOR || c == WINDOWS_PATH_SEPARATOR) {
      return false;
    }
  }
  return true;
}

// Whether normalization would return `path` unchanged: no empty segment
// outside a leading separator, no "." or ".." segment, no trailing
// separator. Joins hand their result back in this spelling, and a walk
// that has nothing to fold only copies both halves to say the same thing.
bool is_canonical(std::string_view path) {
  if (path.empty() || path.back() == DEFAULT_PATH_SEPARATOR) {
    return false;
  }
  usize start = 0;
  for (usize i = 0; i <= path.size(); ++i) {
    if (i < path.size() && path[i] != DEFAULT_PATH_SEPARATOR) {
      continue;
    }
    const std::string_view segment = path.substr(start, i - start);
    if (segment.empty()) {
      if (start != 0) {
        return false;
      }
    } else if (segment.size() <= 2 && (segment == "." || segment == "..")) {
      return false;
    }
    start = i + 1;
  }
  return true;
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
  if (path.find(WINDOWS_PATH_SEPARATOR) != std::string_view::npos) {
    return base::make_ok(Path(normalize_canonical(fold_separators(path))));
  }
#endif
  if (is_canonical(path)) {
    return base::make_ok(Path(std::string(path)));
  }
  return base::make_ok(Path(normalize_canonical(path)));
}

Path Path::join(std::string_view child) const {
  if (is_plain_name(child) && !value_.empty() && value_ != "." &&
      value_.back() != DEFAULT_PATH_SEPARATOR) {
    std::string out;
    out.reserve(value_.size() + child.size() + 1);
    out.append(value_);
    out.push_back(DEFAULT_PATH_SEPARATOR);
    out.append(child);
    return Path(std::move(out));
  }
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
  if (value_.empty()) {
    return false;
  }
  if (value_.front() == DEFAULT_PATH_SEPARATOR) {
    return true;
  }
#if BUILD_FLAG(IS_OS_WIN)
  // A drive root (`C:/x`) and a UNC root (`//host/share`) are absolute
  // too, yet neither starts with a separator.
  if (value_.size() >= 2 && value_[1] == ':') {
    return true;
  }
  return value_.starts_with("//");
#else
  // Off Windows a `C:` is just a character in a file name, so only a
  // leading separator makes a path absolute.
  return false;
#endif
}

bool operator==(std::string_view lhs, const Path& rhs) {
  return lhs == rhs.as_view();
}

}  // namespace path
