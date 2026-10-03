// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/trace.h"

#include "fpag/debug/profiler/profiler.h"
#include "pipeline/pipeline_context.h"

namespace cli {

TraceSession::TraceSession(pipeline::PipelineContext& ctx, bool enabled)
    : ctx_(ctx), enabled_(enabled) {
  if (enabled_) {
    debug::Profiler::global().clear();
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

TraceCapture TraceSession::take_events() {
  done_ = true;
  if (!enabled_) {
    return {};
  }
  debug::Profiler::global().stop();
  // The global outlives every render of the capture, which is what lets
  // the envelope own the events without owning the interner.
  return {debug::Profiler::global().copy_events(), &debug::Profiler::global()};
}

}  // namespace cli
