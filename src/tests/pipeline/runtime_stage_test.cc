// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/runtime_stage.h"

#include <optional>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "fpag/mem/arena.h"
#include "pipeline/embedded_runtime.h"
#include "source/source.h"

namespace pipeline {

namespace {

TEST_CASE("Stage writes embedded runtime sources") {
  io::TempDir dir = io::TempDir::create_unique("alcy_runtime_stage_test_");
  mem::Arena arena;
  arena.reserve(1u << 16);
  diag::DiagBag bag{arena};
  CHECK(stage_runtime(dir, bag).is_ok());
  CHECK(!bag.has_errors());

  source::SourceManager sources;
  base::Result<source::FileId, source::SourceError> header =
      sources.load(dir.join(runtime_header_name()));
  CHECK(header.is_ok());
  base::Result<source::FileId, source::SourceError> source =
      sources.load(dir.join(runtime_source_name()));
  CHECK(source.is_ok());
  if (header.is_err() || source.is_err()) {
    return;
  }
  const std::optional<std::string_view> staged_header =
      sources.bytes(std::move(header).unwrap());
  const std::optional<std::string_view> staged_source =
      sources.bytes(std::move(source).unwrap());
  CHECK(staged_header.has_value());
  CHECK(staged_source.has_value());
  if (!staged_header.has_value() || !staged_source.has_value()) {
    return;
  }
  CHECK(*staged_header ==
        std::string_view(reinterpret_cast<const char*>(ALCY_RUNTIME_HEADER),
                         ALCY_RUNTIME_HEADER_LEN));
  CHECK(*staged_source ==
        std::string_view(reinterpret_cast<const char*>(ALCY_RUNTIME_SOURCE),
                         ALCY_RUNTIME_SOURCE_LEN));
  // The staged header is a real translation-unit dependency.
  CHECK(staged_source->find("alcy_runtime.h") != std::string_view::npos);
}

}  // namespace

}  // namespace pipeline
