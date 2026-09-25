// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/diagnostic_output.h"

#include <string_view>

#include "base/logger.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "fmt/format.h"
#include "source/source.h"

namespace cli {

namespace {

diag::SourceText fetch_source(source::FileId id, const void* ctx) {
  const auto* sources = static_cast<const source::SourceManager*>(ctx);
  if (id >= sources->file_count()) {
    return {};
  }
  return {sources->name(id), sources->bytes(id)};
}

}  // namespace

void report_diagnostics(const diag::DiagBag& bag,
                        const source::SourceManager& sources,
                        const diag::RenderOptions& options) {
  bag.for_each([&](const diag::Diagnostic& diag) {
    fmt::memory_buffer out;
    diag::render(diag, out, options, fetch_source, &sources);
    base::logger.wo_prefix("{}", std::string_view(out.data(), out.size()));
  });
}

}  // namespace cli
