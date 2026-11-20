// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/paths.h"

#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"

namespace ssg {

std::string trim_view(std::string_view text) {
  usize start = 0;
  usize end = text.size();
  while (start < end && (text[start] == ' ' || text[start] == '\t')) {
    start++;
  }
  while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
    end--;
  }
  return std::string(text.substr(start, end - start));
}

std::string lowercase(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const u8 c : text) {
    out += static_cast<char>(std::tolower(c));
  }
  return out;
}

std::vector<std::string_view> split_segments(std::string_view path) {
  std::vector<std::string_view> segments;
  usize begin = 0;
  while (begin < path.size()) {
    usize end = path.find('/', begin);
    if (end == std::string_view::npos) {
      end = path.size();
    }
    if (end > begin) {
      const std::string_view part = path.substr(begin, end - begin);
      if (part != ".") {
        segments.push_back(part);
      }
    }
    begin = end + 1;
  }
  return segments;
}

std::string normalize_posix(std::string_view path) {
  if (path.empty()) {
    return std::string();
  }
  const bool absolute = path.front() == '/';
  const bool trailing = path.back() == '/';
  std::vector<std::string_view> segments;
  for (const std::string_view part : split_segments(path)) {
    if (part == ".") {
      continue;
    }
    if (part.empty()) {
      continue;
    }
    if (part == "..") {
      if (!segments.empty() && segments.back() != "..") {
        segments.pop_back();
      } else if (!absolute) {
        segments.push_back(part);
      }
      continue;
    }
    segments.push_back(part);
  }
  std::string out = absolute ? "/" : std::string();
  for (usize index = 0; index < segments.size(); index++) {
    out += segments[index];
    if (index + 1 < segments.size() || trailing) {
      out += '/';
    }
  }
  if (out.empty() && !absolute) {
    return ".";
  }
  return out;
}

std::string join_posix(std::string_view first, std::string_view second) {
  if (first.empty()) {
    return normalize_posix(second);
  }
  if (second.empty()) {
    return normalize_posix(first);
  }
  // A path rooted at / answers for itself: joining an absolute
  // entry onto a base keeps the entry verbatim.
  if (second.front() == '/') {
    return normalize_posix(second);
  }
  std::string joined;
  joined.reserve(first.size() + second.size() + 1);
  joined = first;
  if (first.back() == '/' && second.front() == '/') {
    joined =
        normalize_posix(std::string(first) + std::string(second.substr(1)));
    return joined;
  }
  if (first.back() != '/' && second.front() != '/') {
    joined += '/';
  }
  joined += second;
  return normalize_posix(joined);
}

std::string join_posix(std::initializer_list<std::string_view> parts) {
  std::string out;
  for (const std::string_view part : parts) {
    if (out.empty()) {
      out = std::string(part);
      continue;
    }
    out = join_posix(out, part);
  }
  return out;
}

std::string dirname_posix(std::string_view path) {
  const std::string normalized = normalize_posix(path);
  const usize slash = normalized.rfind('/');
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return "/";
  }
  return normalized.substr(0, slash);
}

std::string basename_posix(std::string_view path) {
  const std::string normalized = normalize_posix(path);
  const usize slash = normalized.rfind('/');
  return slash == std::string::npos ? normalized : normalized.substr(slash + 1);
}

std::string extension_posix(std::string_view path) {
  const std::string base = basename_posix(path);
  const usize dot = base.rfind('.');
  return dot == std::string::npos ? std::string() : base.substr(dot + 1);
}

std::string relative_posix(std::string_view from_dir,
                           std::string_view to_path) {
  const std::vector<std::string_view> from = split_segments(from_dir);
  const std::vector<std::string_view> to = split_segments(to_path);
  usize common = 0;
  while (common < from.size() && common < to.size() &&
         from[common] == to[common]) {
    common++;
  }
  std::string out;
  for (usize index = common; index < from.size(); index++) {
    out += "../";
  }
  for (usize index = common; index < to.size(); index++) {
    if (!out.empty() && out.back() != '/') {
      out += '/';
    }
    out += to[index];
  }
  return out;
}

}  // namespace ssg
