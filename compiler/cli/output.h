// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cli/logger.h"
#include "cli/trace.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/profiler/profile_event.h"
#include "fpag/term/color_mode.h"
#include "i18n/language.h"

namespace diag {

class DiagBag;

}  // namespace diag

namespace source {

class SourceManager;

}  // namespace source

namespace cli {

// What a command did, as a word rather than a sentence. The renderer owns
// the wording so that it lives in one place, and a tool reading the JSON
// gets a stable name instead of an English string it has to parse.
enum class Outcome : u8 {
  Failed,
  Built,
  Compiled,
  Checked,
  Ran,
  CreatedPackage,
};

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
  // What the command did, or `Failed` if it did not. The default is
  // failure, so a path that returns early without saying what it reached
  // says it reached nothing.
  Outcome outcome = Outcome::Failed;
  // Why a command failed, for a failure raised before any context existed
  // to hold a bag: a rejected flag combination is the only kind left. It
  // is a diagnostic all the same, so it renders through the marker every
  // other error uses, and it carries no code because no check area
  // allocated one for a command line.
  //
  // The message is a view of text the producer keeps alive for the
  // report, on the same terms as the bag below. Absent for a failure the
  // bag already explains, and for every success.
  std::optional<diag::Diagnostic> failure;
  // What the command wrote, as the caller would name it. Empty when it
  // wrote nothing, which is what a check does.
  std::string output_path;
  // What a scaffold created, and where. Only set by `new` and `init`.
  std::string package_name;
  std::string package_dir;
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
  // keep alive, and carried with the profiler that resolves their names.
  TraceCapture trace;
};

// The diagnostics, as one block: everything in the bag, then a failure
// the envelope carries itself. Empty when there is nothing to report,
// which is what a command that succeeded quietly has.
//
// Standard error, because a reader who asked for the result does not want
// the complaints in the same stream: `alcy check > report.txt` keeps a
// clean file, and `alcy run | grep` never sees them at all.
std::string render_diagnostics(const Envelope& envelope,
                               const diag::RenderOptions& r);

// The result line and the time-trace summary, as one block. Empty for a
// failure the diagnostics already explain, and for a run, which announced
// itself before the program started.
std::string render_result(const Envelope& envelope,
                          const diag::RenderOptions& r);

// Writes the line that labels what a command is about to do, to
// `err`, immediately before the command does it.
//
// Standard error because standard output belongs to whatever the
// command is about to run: `alcy run | grep` sees the program's output
// and nothing else. The options carry the capability of the stream
// actually written to, so a label on a redirected standard error is
// plain while one on a terminal is not.
void announce(const Logger& err,
              std::string_view verb,
              std::string_view subject,
              const diag::RenderOptions& options);

// One JSON document: the result, its diagnostics, and the trace when
// there is one. The trace's `traceEvents` sits at the top level so the
// document can be pasted into a trace viewer unchanged.
std::string render_json(const Envelope& envelope, i18n::Language language);

// Writes a finished envelope: the diagnostics to `err` and the result to
// `out`, as text, or the whole thing to `out` as one JSON document when
// the invocation asked for it. This is the only exit for command results,
// which is what keeps "standard output is exactly one JSON document" true
// without any command knowing that JSON exists.
//
// The colour mode is a mode rather than a decision, because this is where
// the decision is made: each half is styled by the stream it lands on, so
// redirecting one of them does not decide whether the other is readable.
void report(const Logger& out,
            const Logger& err,
            const Envelope& envelope,
            term::ColorMode color_mode,
            i18n::Language language,
            bool json);

// Elapsed time since start, in nanoseconds, on a monotonic clock. One
// call per phase boundary is the whole measurement.
u64 elapsed_ns_since(std::chrono::steady_clock::time_point start);

}  // namespace cli
