// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "pipeline/pipeline_context.h"

namespace cli {

// A --time-trace session for one command. While alive it points the
// context at the global profiler; finish() stops the profiler and
// writes its events as Chromium trace JSON to the path. The
// destructor aborts an unfinished session, so early returns never
// leak a running profiler into the next command in the process.
class TraceSession {
 public:
  TraceSession(pipeline::PipelineContext& ctx, bool enabled);
  ~TraceSession();

  TraceSession(const TraceSession&) = delete;
  TraceSession& operator=(const TraceSession&) = delete;

  void set_path(std::string path);
  std::string_view path() const { return path_; }

  // Writes the trace. True when tracing was off or the write
  // succeeded; false names a write the caller must report.
  bool finish();

 private:
  pipeline::PipelineContext& ctx_;
  bool enabled_;
  bool done_ = false;
  std::string path_ = "trace.json";
};

// Trace path beside an output: `<output>.trace.json`, or `trace.json`
// in the working directory when no output was named.
std::string trace_path_beside(std::string_view output);

}  // namespace cli
