// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/trace.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fpag/debug/profiler/profile_event.h"
#include "fpag/debug/profiler/profiler.h"
#include "fpag/debug/profiler/time_trace_formatter.h"
#include "pipeline/pipeline_context.h"

namespace cli {

TraceSession::TraceSession(pipeline::PipelineContext& ctx, bool enabled)
    : ctx_(ctx), enabled_(enabled) {
  if (enabled_) {
    debug::Profiler::global().start();
    ctx_.profiler = &debug::Profiler::global();
  }
}

TraceSession::~TraceSession() {
  ctx_.profiler = nullptr;
  if (enabled_ && !done_) {
    debug::Profiler::global().stop();
  }
}

void TraceSession::set_path(std::string path) {
  path_ = std::move(path);
}

bool TraceSession::finish() {
  done_ = true;
  if (!enabled_) {
    return true;
  }
  debug::Profiler::global().stop();
  const std::vector<debug::ProfileEvent> events =
      debug::Profiler::global().copy_events();
  return debug::TimeTraceFormatter::write_to_file(path_, events);
}

std::string trace_path_beside(std::string_view output) {
  if (output.empty()) {
    return "trace.json";
  }
  std::string path(output);
  path += ".trace.json";
  return path;
}

}  // namespace cli
