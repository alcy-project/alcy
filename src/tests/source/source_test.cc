// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "source/source.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "config/build_config.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"

#if BUILD_FLAG(IS_OS_POSIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace source {

TEST_CASE("SourceManager loads files and dedups by path") {
  io::TempDir dir = io::TempDir::create_unique("alcy_source_test_");
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
  const std::optional<std::string_view> bytes = sources.bytes(id);
  const std::optional<std::string_view> name = sources.name(id);
  CHECK(bytes.has_value());
  CHECK(name.has_value());
  if (!bytes.has_value() || !name.has_value()) {
    return;
  }
  CHECK(*bytes == "let x = 1;\n");
  CHECK(*name == dir.join("a.al"));
  CHECK(sources.file_count() == 1);

  // An id that names no loaded file is nullopt, not an empty view.
  CHECK(!sources.bytes(id + 1).has_value());
  CHECK(!sources.name(id + 1).has_value());

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
  CHECK(std::move(result).unwrap_err() == SourceError::NotFound);
}

#if BUILD_FLAG(IS_OS_POSIX)
TEST_CASE("SourceManager reports unreadable files") {
  if (::geteuid() == 0) {
    // Root reads past file permissions, so the case cannot be posed.
    return;
  }
  io::TempDir dir = io::TempDir::create_unique("alcy_source_denied_test_");
  const bool written = dir.write_file("denied.al", "let x = 1;\n");
  CHECK(written);
  if (!written) {
    return;
  }
  const std::string path = dir.join("denied.al");
  CHECK(::chmod(path.c_str(), 0) == 0);

  SourceManager sources;
  base::Result<FileId, SourceError> result = sources.load(path);
  CHECK(result.is_err());
  if (!result.is_err()) {
    return;
  }
  CHECK(std::move(result).unwrap_err() == SourceError::OpenFailed);
}
#endif

TEST_CASE("SourceManager loads empty files") {
  io::TempDir dir = io::TempDir::create_unique("alcy_source_empty_test_");
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
  // A loaded empty file is known: engaged, with an empty view.
  const std::optional<std::string_view> bytes =
      sources.bytes(std::move(result).unwrap());
  CHECK(bytes.has_value());
  if (!bytes.has_value()) {
    return;
  }
  CHECK(bytes->empty());
}

#if BUILD_FLAG(IS_OS_POSIX)
TEST_CASE("SourceManager refuses a non-regular file") {
  // A device reports no size and cannot be mapped, so loading one as
  // empty would check a program out as nothing.
  SourceManager sources;
  base::Result<FileId, SourceError> device = sources.load("/dev/null");
  CHECK(device.is_err());
  if (device.is_err()) {
    CHECK(std::move(device).unwrap_err() == SourceError::NotRegular);
  }
}
#endif

}  // namespace source
