// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/trace.h"

#include <chrono>
#include <thread>
#include <utility>

#include "base/process_memory.h"
#include "fpag/debug/profiler/profiler.h"
#include "fpag/debug/time_util.h"
#include "pipeline/pipeline_context.h"

namespace cli {

TraceSession::TraceSession(pipeline::PipelineContext& ctx, bool enabled)
    : ctx_(ctx), enabled_(enabled) {
  if (enabled_) {
    debug::Profiler::global().clear();
    debug::Profiler::global().start();
    ctx_.profiler = &debug::Profiler::global();
    // The resident set is sampled on a cadence while the command runs,
    // which turns the peak the envelope prints into a curve the trace
    // viewers draw over the phases. The thread writes only samples_, and
    // the samples are read after join, so no lock guards them.
    sampling_ = true;
    sampler_ = std::thread([this] {
      while (sampling_) {
        const u64 bytes = base::current_memory_bytes();
        if (bytes != 0) {
          samples_.push_back(
              MemorySample{debug::current_timestamp_ns(), bytes});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    });
  }
}

TraceSession::~TraceSession() {
  stop_sampling();
  ctx_.profiler = nullptr;
}

void TraceSession::stop_sampling() {
  if (sampling_.exchange(false)) {
    sampler_.join();
  }
}

TraceCapture TraceSession::take_events() {
  done_ = true;
  stop_sampling();
  if (!enabled_) {
    return {};
  }
  debug::Profiler::global().stop();
  // The global outlives every render of the capture, which is what lets
  // the envelope own the events without owning the interner. The
  // samples stopped with the session, so they move out whole.
  return {debug::Profiler::global().copy_events(), std::move(samples_),
          &debug::Profiler::global()};
}

}  // namespace cli
