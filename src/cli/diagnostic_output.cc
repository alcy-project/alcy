// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/diagnostic_output.h"

#include <optional>
#include <string_view>

#include "base/logger.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "fmt/format.h"
#include "source/source.h"

namespace cli {

namespace {

std::optional<diag::SourceText> fetch_source(source::FileId id,
                                             const void* ctx) {
  const auto* sources = static_cast<const source::SourceManager*>(ctx);
  const std::optional<std::string_view> name = sources->name(id);
  const std::optional<std::string_view> bytes = sources->bytes(id);
  if (!name.has_value() || !bytes.has_value()) {
    return std::nullopt;
  }
  return diag::SourceText{*name, *bytes};
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
