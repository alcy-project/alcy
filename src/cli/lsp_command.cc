// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/lsp_command.h"

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>

#include "cli/cli_config.h"
#include "cli/output.h"
#include "fpag/base/numeric.h"
#include "lsp/lsp.h"
#include "pipeline/pipeline_context.h"
#include "tcp/endpoint.h"

namespace cli {

namespace {

bool read_port(std::string_view text, u16* out) {
  u32 value = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, value);
  if (result.ptr != end || result.ec != std::errc{} || value == 0 ||
      value > 65535) {
    return false;
  }
  *out = static_cast<u16>(value);
  return true;
}

}  // namespace

i32 run_lsp(const CliConfig& config, pipeline::PipelineContext&, Envelope&) {
  if (config.lsp_transport == CliConfig::LspTransport::Stdio) {
    return lsp::run(lsp::RunOptions{});
  }
  if (!tcp::is_loopback(config.lsp_host)) {
    std::fprintf(
        stderr, "alcy lsp: only loopback addresses are accepted, got '%.*s'\n",
        static_cast<i32>(config.lsp_host.size()), config.lsp_host.data());
    return 1;
  }
  u16 port = 0;
  if (!read_port(config.lsp_port, &port)) {
    std::fprintf(stderr, "alcy lsp: --port must be a number from 1 to 65535\n");
    return 1;
  }
  lsp::RunOptions options;
  options.transport = lsp::RunOptions::TransportKind::Tcp;
  options.endpoint = tcp::Endpoint{std::string(config.lsp_host), port};
  return lsp::run(options);
}

}  // namespace cli
