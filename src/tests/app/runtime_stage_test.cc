// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "app/runtime_stage.h"

#include <string_view>
#include <utility>

#include "app/embedded_runtime.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "fpag/mem/arena.h"
#include "source/source.h"

namespace app {

namespace {

TEST_CASE("Stage writes embedded runtime sources") {
  io::TempDir dir("alcy_runtime_stage_test");
  CHECK(stage_runtime(dir));

  mem::Arena arena;
  arena.reserve(1u << 16);
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
  const std::string_view staged_header =
      sources.bytes(std::move(header).unwrap());
  const std::string_view staged_source =
      sources.bytes(std::move(source).unwrap());
  CHECK(staged_header ==
        std::string_view(reinterpret_cast<const char*>(kAlcyRuntimeHeader),
                         kAlcyRuntimeHeader_len));
  CHECK(staged_source ==
        std::string_view(reinterpret_cast<const char*>(kAlcyRuntimeSource),
                         kAlcyRuntimeSource_len));
  // The staged header is a real translation-unit dependency.
  CHECK(staged_source.find("alcy_runtime.h") != std::string_view::npos);
}

}  // namespace

}  // namespace app
