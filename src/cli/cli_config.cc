// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/cli_config.h"

#include <span>
#include <string_view>

#include "codegen/backend.h"
#include "pipeline/backend_emit.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/profile.h"
#include "pkg/toolchain.h"

namespace cli {

bool force_freestanding_elf(std::string_view triple) {
  if (triple.find("linux") == std::string_view::npos) {
    return false;
  }
  return triple.rfind("x86_64", 0) == 0 || triple.rfind("aarch64", 0) == 0 ||
         triple.rfind("arm64", 0) == 0 || triple.rfind("riscv64", 0) == 0;
}

pipeline::LinkOptions resolve_link_options(const CliConfig& config,
                                           const pkg::Toolchain& tool,
                                           std::string_view /*target*/) {
  // A flag replaces the file's list instead of adding to it, the same way
  // an explicit driver replaces the file's driver. A list half from each
  // could be predicted from neither.
  std::span<const std::string_view> args = tool.link_args;
  if (!config.link_args.empty()) {
    args = config.link_args;
  }
  return pipeline::LinkOptions{
      .driver = config.linker.empty() ? tool.linker : config.linker,
      .args = args,
      .freestanding = tool.freestanding};
}

void resolve_backend_options(const CliConfig& config,
                             pipeline::PipelineContext& ctx,
                             const pipeline::BuildProfile& profile) {
  // A named backend stands, even when the profile optimizes and that
  // backend cannot: the emitter's own refusal names the contradiction
  // better than a silent substitution would.
  if (config.backend != codegen::Backend::None) {
    ctx.backend = config.backend;
    return;
  }
  ctx.backend = pipeline::select_backend(
      ctx.target, pipeline::output_kind_for(config.emit, ctx.target),
      profile.optimize);
}

}  // namespace cli
