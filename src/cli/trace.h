// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <atomic>
#include <thread>
#include <vector>

#include "fpag/debug/profiler/profile_event.h"
#include "fpag/debug/profiler/profiler.h"
#include "pipeline/pipeline_context.h"

namespace cli {

// One resident-set reading taken on a cadence while the command ran:
// the counter series the trace viewers plot over the phases.
struct MemorySample {
  u64 time_ns = 0;
  u64 bytes = 0;
};

// A recorded trace and the profiler that resolves it. An event names its
// phase and category with ids into that profiler rather than with text,
// so the two travel together.
struct TraceCapture {
  std::vector<debug::ProfileEvent> events;
  std::vector<MemorySample> memory_samples;
  const debug::Profiler* profiler = nullptr;
};

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
  TraceCapture take_events();

 private:
  // Stops the sampler and joins its thread. Idempotent; every exit path
  // calls it before the samples are read.
  void stop_sampling();

  pipeline::PipelineContext& ctx_;
  bool enabled_;
  bool done_ = false;
  std::atomic<bool> sampling_{false};
  std::thread sampler_;
  std::vector<MemorySample> samples_;
};

}  // namespace cli
