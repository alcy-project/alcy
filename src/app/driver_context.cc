// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "app/driver_context.h"

#include <string>
#include <string_view>

#include "base/logger.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "fmt/format.h"
#include "pipeline/pipeline.h"
#include "source/source.h"

namespace app {

DriverContext::DriverContext() : bag(arena), strings(mem::page_size()) {
  arena.reserve(1u << 20);
}

void report(const diag::DiagBag& bag, const source::SourceManager& sources) {
  bag.for_each([&](const diag::Diagnostic& diag) {
    fmt::memory_buffer out;
    diag::render(diag, out, {}, pipeline::fetch_source, &sources);
    base::logger.wo_prefix("{}", std::string_view(out.data(), out.size()));
  });
}

}  // namespace app
