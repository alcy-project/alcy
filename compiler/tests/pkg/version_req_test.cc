// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/version_req.h"

#include <initializer_list>
#include <string_view>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace pkg {

namespace {

VersionReq bounds(std::initializer_list<VersionBound> list) {
  VersionReq req;
  for (const VersionBound& bound : list) {
    req.bounds[req.count++] = bound;
  }
  return req;
}

TEST_CASE("A full version is one exact bound") {
  CHECK(parse_version_req("1.2.3").unwrap() ==
        bounds({{VersionOp::Equal, {1, 2, 3}}}));
  CHECK(parse_version_req("=1.2.3").unwrap() ==
        bounds({{VersionOp::Equal, {1, 2, 3}}}));
  CHECK(parse_version_req("12.34.56").unwrap() ==
        bounds({{VersionOp::Equal, {12, 34, 56}}}));
}

TEST_CASE("A wildcard expands to the series it names") {
  CHECK(parse_version_req("1.2.x").unwrap() ==
        bounds({{VersionOp::GreaterEqual, {1, 2, 0}},
                {VersionOp::Less, {1, 3, 0}}}));
  CHECK(parse_version_req("1.x").unwrap() ==
        bounds({{VersionOp::GreaterEqual, {1, 0, 0}},
                {VersionOp::Less, {2, 0, 0}}}));
  // `0.x` is a major series like any other; whether it is worth
  // discouraging is a manifest lint's question (ADR-0057).
  CHECK(parse_version_req("0.x").unwrap() ==
        bounds({{VersionOp::GreaterEqual, {0, 0, 0}},
                {VersionOp::Less, {1, 0, 0}}}));
}

TEST_CASE("Comparators keep the comparison they name") {
  CHECK(parse_version_req(">1.2.3").unwrap() ==
        bounds({{VersionOp::Greater, {1, 2, 3}}}));
  CHECK(parse_version_req(">=1.2.3").unwrap() ==
        bounds({{VersionOp::GreaterEqual, {1, 2, 3}}}));
  CHECK(parse_version_req("<1.2.3").unwrap() ==
        bounds({{VersionOp::Less, {1, 2, 3}}}));
  CHECK(parse_version_req("<=1.2.3").unwrap() ==
        bounds({{VersionOp::LessEqual, {1, 2, 3}}}));
  CHECK(parse_version_req(" >= 1.2.3 ").unwrap() ==
        bounds({{VersionOp::GreaterEqual, {1, 2, 3}}}));
}

TEST_CASE("Commas are conjunction") {
  const VersionReq req = parse_version_req(">=1.2.0, <1.5.0").unwrap();
  CHECK(req == bounds({{VersionOp::GreaterEqual, {1, 2, 0}},
                       {VersionOp::Less, {1, 5, 0}}}));
  CHECK(parse_version_req(">=1.2.0,<1.5.0").unwrap() == req);
  CHECK(parse_version_req("1.2.x, >=1.2.3").unwrap() ==
        bounds({{VersionOp::GreaterEqual, {1, 2, 0}},
                {VersionOp::Less, {1, 3, 0}},
                {VersionOp::GreaterEqual, {1, 2, 3}}}));
}

TEST_CASE("The grammar refuses what it does not accept") {
  CHECK(parse_version_req("").unwrap_err() == VersionReqError::Empty);
  CHECK(parse_version_req("  ").unwrap_err() == VersionReqError::Empty);
  for (const std::string_view bad : {"x",
                                     "*",
                                     "1",
                                     "1.2",
                                     "^1.2.3",
                                     "~1.2.3",
                                     ">=1.2",
                                     ">=1.2.x",
                                     "=1.2.x",
                                     "X.Y.x",
                                     "1.x.x",
                                     "1.2.X",
                                     "x.2.3",
                                     "1.x.3",
                                     "1.2.3.4",
                                     "1..3",
                                     "1.2.",
                                     ",1.2.3",
                                     "1.2.3,",
                                     "1.2.3,,",
                                     "> =1.2.3",
                                     "4294967296.0.0",
                                     "1.4294967295.x"}) {
    CHECK(parse_version_req(bad).unwrap_err() == VersionReqError::BadFormat);
  }
}

TEST_CASE("More than the accepted bounds is its own error") {
  CHECK(parse_version_req("1.x,2.x,3.x,4.x,").unwrap_err() ==
        VersionReqError::BadFormat);
  CHECK(parse_version_req("1.x,2.x,3.x,4.x,5.x").unwrap_err() ==
        VersionReqError::TooManyBounds);
}

}  // namespace

}  // namespace pkg
