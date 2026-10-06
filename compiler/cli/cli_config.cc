// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/cli_config.h"

#include <span>
#include <string_view>

#include "pipeline/link_options.h"
#include "pkg/toolchain.h"

namespace cli {

pipeline::LinkOptions resolve_link_options(const CliConfig& config,
                                           const pkg::Toolchain& tool) {
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

}  // namespace cli
