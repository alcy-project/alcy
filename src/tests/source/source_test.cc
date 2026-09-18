// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "source/source.h"

#include <string_view>
#include <utility>

#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "tests/util/test_fs.h"

namespace source {

// NOTE: doctest REQUIRE needs exceptions (disabled here), so setup steps
// use CHECK plus an early return instead.
TEST_CASE("SourceManager loads files and dedups by path") {
  test_fs::TempDir dir("alcy_source_test");
  const bool written = dir.write_file("a.al", "let x = 1;\n");
  CHECK(written);
  if (!written) {
    return;
  }

  SourceManager sources;
  base::Result<FileId, SourceError> first = sources.load(dir.join("a.al"));
  CHECK(first.is_ok());
  if (!first.is_ok()) {
    return;
  }
  const FileId id = std::move(first).unwrap();
  CHECK(sources.bytes(id) == "let x = 1;\n");
  CHECK(sources.name(id) == dir.join("a.al"));
  CHECK(sources.file_count() == 1);

  base::Result<FileId, SourceError> second = sources.load(dir.join("a.al"));
  CHECK(second.is_ok());
  if (!second.is_ok()) {
    return;
  }
  CHECK(std::move(second).unwrap() == id);
  CHECK(sources.file_count() == 1);
}

TEST_CASE("SourceManager reports missing files") {
  SourceManager sources;
  base::Result<FileId, SourceError> result =
      sources.load("/nonexistent-dir-xyz/missing.al");
  CHECK(result.is_err());
  if (!result.is_err()) {
    return;
  }
  CHECK(std::move(result).unwrap_err() == SourceError::OpenFailed);
}

TEST_CASE("SourceManager loads empty files") {
  test_fs::TempDir dir("alcy_source_empty_test");
  const bool written = dir.write_file("empty.al", "");
  CHECK(written);
  if (!written) {
    return;
  }

  SourceManager sources;
  base::Result<FileId, SourceError> result = sources.load(dir.join("empty.al"));
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  CHECK(sources.bytes(std::move(result).unwrap()).empty());
}

}  // namespace source
