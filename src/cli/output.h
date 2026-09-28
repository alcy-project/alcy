// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "diag/render.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/profiler/profile_event.h"

namespace diag {

class DiagBag;

}  // namespace diag

namespace source {

class SourceManager;

}  // namespace source

namespace cli {

// Whether the command did what it was asked to do. A command sets it
// once, at its success exit; the default is failure, so a path that
// returns early reports an error it already put in the bag.
enum class Status : u8 {
  Ok,
  Error,
};

// What one command did. Filled in as the command runs and rendered once,
// at the end, by whichever renderer the invocation asked for.
struct Envelope {
  // The verb, as spelled on the command line.
  std::string_view command;
  Status status = Status::Error;
  // What the command is called in a sentence: "built", "checked",
  // "created package". Empty when there is nothing to say.
  std::string_view summary;
  // Counts worth reporting. Zero means the command has no such
  // measurement, which is why they are not optional: a package with no
  // files and a package that was not counted read the same, and only one
  // of them is a bug worth reporting.
  usize file_count = 0;
  usize module_count = 0;
  usize function_count = 0;
  // Size of what was written, or zero when the command wrote nothing.
  u64 output_bytes = 0;
  // Wall time of the whole invocation, measured by the caller with a
  // monotonic clock. The profiler's clock is wall-clock derived and is
  // not used for it.
  u64 wall_ns = 0;
  // Diagnostics in emission order, borrowed from the invocation's
  // context. Both are null until a command has one, which is also the
  // case for a failure raised before any context exists.
  const diag::DiagBag* bag = nullptr;
  const source::SourceManager* sources = nullptr;
  // Recorded phases, empty unless --time-trace ran. Copied rather than
  // borrowed because the profiler's storage is not the envelope's to
  // keep alive.
  std::vector<debug::ProfileEvent> trace;
};

// Diagnostics, the result line, and the time-trace summary, as text.
std::string render_text(const Envelope& envelope, const diag::RenderOptions& r);

// One JSON document: the result, its diagnostics, and the trace when
// there is one. The trace's `traceEvents` sits at the top level so the
// document can be pasted into a trace viewer unchanged.
std::string render_json(const Envelope& envelope);

// Appends a JSON string literal for text, quotes and escaping included.
void append_json_string(std::string& out, std::string_view text);

// Writes a finished envelope to standard output: the text report, or
// the JSON document when the invocation asked for it. This is the only
// exit for command results, which is what keeps "standard output is
// exactly one JSON document" true without any command knowing that
// JSON exists.
void report(const Envelope& envelope,
            const diag::RenderOptions& options,
            bool json);

// Elapsed time since start, in nanoseconds, on a monotonic clock. One
// call per phase boundary is the whole measurement.
u64 elapsed_ns_since(std::chrono::steady_clock::time_point start);

}  // namespace cli
