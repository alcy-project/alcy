// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "path/path.h"

#include <string>
#include <utility>

#include "doctest/doctest.h"
#include "fpag/base/result.h"

namespace path {

TEST_CASE("Path normalizes duplicates dots and trailing slashes") {
  CHECK(Path::from_native("a//b/./c/").unwrap().as_view() == "a/b/c");
  CHECK(Path::from_native("a/b/../c").unwrap().as_view() == "a/c");
  CHECK(Path::from_native("/a/../../b").unwrap().as_view() == "/b");
  CHECK(Path::from_native("").unwrap().as_view() == ".");
  CHECK(Path::from_native(".").unwrap().as_view() == ".");
  CHECK(Path::from_native("/").unwrap().as_view() == "/");
}

TEST_CASE("Path rejects embedded NUL bytes") {
  const std::string evil("a\0b", 3);
  base::Result<Path, PathError> result = Path::from_native(evil);
  CHECK(result.is_err());
  CHECK(std::move(result).unwrap_err() == PathError::ContainsNul);
}

TEST_CASE("Path joins and stays canonical") {
  const Path base = Path::from_native("pkg").unwrap();
  CHECK(base.join("dep").as_view() == "pkg/dep");
  CHECK(base.join("dep/../other").as_view() == "pkg/other");
  CHECK(base.join("").as_view() == "pkg");
}

TEST_CASE("Path compares spelling variants equally") {
  const Path a = Path::from_native("pkg/./dep").unwrap();
  const Path b = Path::from_native("pkg/dep").unwrap();
  CHECK(a == b);
  CHECK(!(a != b));
  CHECK(a == "pkg/dep");
  CHECK("pkg/dep" == a);
  CHECK(a < Path::from_native("pkg/deq").unwrap());
}

TEST_CASE("Path reports parents") {
  CHECK(Path::from_native("a/b/c").unwrap().parent().as_view() == "a/b");
  CHECK(Path::from_native("a").unwrap().parent().as_view() == ".");
  CHECK(Path::from_native("/a").unwrap().parent().as_view() == "/");
  CHECK(Path::from_native("/").unwrap().parent().as_view() == "/");
  CHECK(Path::from_native(".").unwrap().parent().as_view() == ".");
}

}  // namespace path
