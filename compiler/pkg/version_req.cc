// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/version_req.h"

#include <charconv>
#include <limits>
#include <system_error>

namespace pkg {

namespace {

base::Result<VersionReq, VersionReqError> bad_format() {
  return base::make_err(VersionReqError::BadFormat);
}

void skip_spaces(std::string_view text, usize& at) {
  while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) {
    ++at;
  }
}

bool parse_u32(std::string_view text, u32& out) {
  if (text.empty()) {
    return false;
  }
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, out);
  return ec == std::errc() && ptr == end;
}

// Takes one bound when the requirement has room for it.
bool add_bound(VersionReq& req, VersionOp op, Version version) {
  if (req.count >= VersionReq::MAX_BOUNDS) {
    return false;
  }
  req.bounds[req.count].op = op;
  req.bounds[req.count].version = version;
  ++req.count;
  return true;
}

}  // namespace

base::Result<VersionReq, VersionReqError> parse_version_req(
    std::string_view text) {
  VersionReq req;
  usize at = 0;
  skip_spaces(text, at);
  if (at >= text.size()) {
    return base::make_err(VersionReqError::Empty);
  }
  while (true) {
    // The comparator, when one is written.
    VersionOp op = VersionOp::Equal;
    bool has_op = false;
    if (text[at] == '=') {
      op = VersionOp::Equal;
      has_op = true;
      ++at;
    } else if (text[at] == '>') {
      ++at;
      has_op = true;
      if (at < text.size() && text[at] == '=') {
        op = VersionOp::GreaterEqual;
        ++at;
      } else {
        op = VersionOp::Greater;
      }
    } else if (text[at] == '<') {
      ++at;
      has_op = true;
      if (at < text.size() && text[at] == '=') {
        op = VersionOp::LessEqual;
        ++at;
      } else {
        op = VersionOp::Less;
      }
    }
    skip_spaces(text, at);

    // The version token, up to the next comma or the end.
    const usize start = at;
    while (at < text.size() && text[at] != ',') {
      ++at;
    }
    usize stop = at;
    while (stop > start && (text[stop - 1] == ' ' || text[stop - 1] == '\t')) {
      --stop;
    }
    const std::string_view token = text.substr(start, stop - start);

    // Up to three dot-separated components.
    std::string_view parts[3];
    u32 part_count = 0;
    usize part_at = 0;
    while (true) {
      const usize dot = token.find('.', part_at);
      const usize part_end = dot == std::string_view::npos ? token.size() : dot;
      if (part_count >= 3) {
        return bad_format();
      }
      parts[part_count++] = token.substr(part_at, part_end - part_at);
      if (dot == std::string_view::npos) {
        break;
      }
      part_at = dot + 1;
    }

    if (part_count == 3 && parts[2] == "x") {
      // `X.Y.x`: the patch series.
      u32 major = 0;
      u32 minor = 0;
      if (has_op || !parse_u32(parts[0], major) ||
          !parse_u32(parts[1], minor) ||
          minor == std::numeric_limits<u32>::max()) {
        return bad_format();
      }
      if (!add_bound(req, VersionOp::GreaterEqual, Version{major, minor, 0}) ||
          !add_bound(req, VersionOp::Less, Version{major, minor + 1, 0})) {
        return base::make_err(VersionReqError::TooManyBounds);
      }
    } else if (part_count == 2 && parts[1] == "x") {
      // `X.x`: the major series.
      u32 major = 0;
      if (has_op || !parse_u32(parts[0], major) ||
          major == std::numeric_limits<u32>::max()) {
        return bad_format();
      }
      if (!add_bound(req, VersionOp::GreaterEqual, Version{major, 0, 0}) ||
          !add_bound(req, VersionOp::Less, Version{major + 1, 0, 0})) {
        return base::make_err(VersionReqError::TooManyBounds);
      }
    } else if (part_count == 3) {
      u32 major = 0;
      u32 minor = 0;
      u32 patch = 0;
      if (!parse_u32(parts[0], major) || !parse_u32(parts[1], minor) ||
          !parse_u32(parts[2], patch)) {
        return bad_format();
      }
      if (!add_bound(req, op, Version{major, minor, patch})) {
        return base::make_err(VersionReqError::TooManyBounds);
      }
    } else {
      // `x` alone, a partial version without the marker, and anything
      // else that is not one of the accepted term shapes.
      return bad_format();
    }

    skip_spaces(text, at);
    if (at >= text.size()) {
      break;
    }
    if (text[at] != ',') {
      return bad_format();
    }
    ++at;
    skip_spaces(text, at);
    if (at >= text.size()) {
      return bad_format();
    }
  }
  return base::make_ok(req);
}

}  // namespace pkg
