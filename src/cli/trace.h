// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "fpag/debug/profiler/profile_event.h"
#include "pipeline/pipeline_context.h"

namespace cli {

// A --time-trace session for one command. While alive it points the
// context at the global profiler; take_events() stops the profiler and
// hands over what it recorded, for the result envelope to carry. The
// destructor stops an unfinished session, so early returns never leak a
// running profiler into the next command in the process.
class TraceSession {
 public:
  TraceSession(pipeline::PipelineContext& ctx, bool enabled);
  ~TraceSession();

  TraceSession(const TraceSession&) = delete;
  TraceSession& operator=(TraceSession&) = delete;

  // Stops the profiler and returns its events, newest last. Empty when
  // tracing was off. Safe to call once; later calls return nothing.
  std::vector<debug::ProfileEvent> take_events();

 private:
  pipeline::PipelineContext& ctx_;
  bool enabled_;
  bool done_ = false;
};

}  // namespace cli
