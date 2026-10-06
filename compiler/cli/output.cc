// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/output.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cli/duration.h"
#include "cli/logger.h"
#include "cli/trace_report.h"
#include "debug/dcheck.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/json.h"
#include "diag/render.h"
#include "fmt/core.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/profiler/profile_event.h"
#include "fpag/debug/profiler/profiler.h"
#include "fpag/io/io_util.h"
#include "fpag/str/string_pool_id.h"
#include "fpag/term/color_mode.h"
#include "fpag/term/color_style.h"
#include "fpag/term/console.h"
#include "fpag/term/style.h"
#include "i18n/language.h"
#include "i18n/messages.h"
#include "source/source.h"
#include "text/json.h"

namespace cli {

using text::append_json_number;
using text::append_json_string;

namespace {

std::optional<diag::SourceText> fetch_source(source::FileId id,
                                             const void* ctx) {
  const auto* sources = static_cast<const source::SourceManager*>(ctx);
  const std::optional<std::string_view> name = sources->name(id);
  const std::optional<std::string_view> bytes = sources->bytes(id);
  if (!name.has_value() || !bytes.has_value()) {
    return std::nullopt;
  }
  return diag::SourceText{*name, *bytes};
}

void append_diagnostic_text(std::string& out,
                            const diag::Diagnostic& diagnostic,
                            const source::SourceManager& sources,
                            const diag::RenderOptions& options) {
  fmt::memory_buffer rendered;
  diag::render(diagnostic, rendered, options, fetch_source, &sources);
  out.append(rendered.data(), rendered.size());
}

// A message with no span: the marker and the sentence are the whole of
// it, and the source manager has nothing to add. A span here would mean
// the source was not borrowed where the report renders, which is the
// producer's bug rather than something to paper over.
void append_diagnostic_text(std::string& out,
                            const diag::Diagnostic& diagnostic,
                            const diag::RenderOptions& options) {
  DCHECK(!diagnostic.has_primary_span);
  out += diag::render(diagnostic, options);
}

// An id the profiler never interned points outside its pool, so it is
// answered here. Empty is what an event without a name reports.
std::string_view resolve_name(const debug::Profiler& profiler,
                              str::StringPoolId id) {
  return id == str::INVALID_STRING_POOL_ID ? std::string_view()
                                           : profiler.name(id);
}

void append_trace_text(std::string& out, const Envelope& envelope, bool color) {
  if (envelope.trace.events.empty() || envelope.trace.profiler == nullptr) {
    return;
  }
  // Every share answers against the invocation's wall - the number the
  // result line prints just above the trace - so the percentages in the
  // two blocks agree instead of referring to two nearly equal totals.
  const TraceReport report = build_trace_report(envelope.trace);
  out += '\n';
  out += render_trace_text(report, *envelope.trace.profiler, envelope.wall_ns,
                           color);
}

void append_trace_json(std::string& out, const Envelope& envelope) {
  // A complete event carries a duration rather than a timestamp, and
  // the array sits at the top level because that is what a trace viewer
  // reads.
  out += R"(,"traceEvents":[)";
  const debug::Profiler& profiler = *envelope.trace.profiler;
  for (usize i = 0; i < envelope.trace.events.size(); ++i) {
    const debug::ProfileEvent& event = envelope.trace.events[i];
    if (i > 0) {
      out += ',';
    }
    out += R"({"name":)";
    append_json_string(out, resolve_name(profiler, event.name));
    out += R"(,"cat":)";
    append_json_string(out, resolve_name(profiler, event.category));
    out += R"(,"ph":"X","pid":)";
    append_json_number(out, event.process_id);
    out += R"(,"tid":)";
    append_json_number(out, event.thread_id);
    // Microseconds, which is the unit the trace viewers assume for a
    // complete event, and the call site that recorded it.
    out += R"(,"ts":)";
    append_json_number(out, static_cast<f64>(event.start_time_ns) / 1000.0);
    out += R"(,"dur":)";
    append_json_number(out, static_cast<f64>(event.duration_ns) / 1000.0);
    // The call site that recorded the phase, which is what turns a flat
    // list of names into something to navigate.
    out += R"(,"args":{)";
    out += R"("file":)";
    append_json_string(
        out, event.location.file != nullptr ? event.location.file : "");
    out += R"(,"line":)";
    append_json_number(out, event.location.line);
    out += '}';
    out += '}';
  }
  out += ']';
}

// The verb each outcome reports itself as. Owned here so the wording
// lives in one place and a new outcome cannot add a second phrasing of
// the same result.
std::string_view verb_for(Outcome outcome, i18n::Language language) {
  using i18n::Key;
  switch (outcome) {
    case Outcome::Failed: return i18n::text<Key::CliVerbFailed>(language);
    case Outcome::Built: return i18n::text<Key::CliVerbBuilt>(language);
    case Outcome::Compiled: return i18n::text<Key::CliVerbCompiled>(language);
    case Outcome::Checked: return i18n::text<Key::CliVerbChecked>(language);
    case Outcome::Ran: return i18n::text<Key::CliVerbRan>(language);
    case Outcome::CreatedPackage:
      return i18n::text<Key::CliVerbCreatedPackage>(language);
  }
  return i18n::text<Key::CliVerbFailed>(language);
}

// The JSON spells the same outcome as a name a tool can match on, so a
// consumer never has to read an English sentence to know what ran.
const char* outcome_name(Outcome outcome) {
  switch (outcome) {
    case Outcome::Failed: return "failed";
    case Outcome::Built: return "built";
    case Outcome::Compiled: return "compiled";
    case Outcome::Checked: return "checked";
    case Outcome::Ran: return "ran";
    case Outcome::CreatedPackage: return "created-package";
  }
  return "failed";
}

// Where the subject of a result line starts, so the lines of a session
// read as a column. The longest verb is `Compiled`; a scaffold's
// `Created package` is longer still, and is the one result that is not
// a build of something, so it takes the column with it and lines up on
// the words after it.
constexpr usize VERB_COLUMN = 10;

// Bold bright green for the verb, underline for what it acted on, dim for the
// note. The same palette the diagnostic renderer uses, so a result line and a
// diagnostic agree about what a colour means.
void append_bold_bright_green(std::string& out, std::string_view text) {
  out.append(term::BOLD);
  out.append(term::FG_BRIGHT_GREEN);
  out.append(text);
  out.append(term::RESET);
}

void append_underline(std::string& out, std::string_view text) {
  out.append(term::UNDERLINE);
  out.append(text);
  out.append(term::RESET);
}

void append_dim(std::string& out, std::string_view text) {
  out.append(term::DIM);
  out.append(text);
  out.append(term::RESET);
}

// A count with its noun, pluralized. `1 file(s)` was neither: it reads
// as a placeholder in a sentence and as a mistake in a list. A language
// that carries the count inside its noun gets two messages and picks
// between them, which is why both arrive already composed.
void append_count(std::string& out,
                  usize count,
                  std::string_view one,
                  std::string_view many) {
  fmt::format_to(std::back_inserter(out), "{} {}", count,
                 count == 1 ? one : many);
}

// A size in the largest unit that leaves something in front of the
// point, so `742 bytes` and `16.4 KiB` and never `0.0 MiB`. Exact bytes
// below a kibibyte, which is the only figure worth counting there.
void append_size(std::string& out, u64 bytes, i18n::Language language) {
  constexpr f64 KIB = 1024.0;
  constexpr f64 MIB = KIB * KIB;
  constexpr f64 GIB = MIB * KIB;
  const f64 size = static_cast<f64>(bytes);
  if (size < KIB) {
    fmt::format_to(std::back_inserter(out), "{} {}", bytes,
                   bytes == 1 ? i18n::text<i18n::Key::CliByteSingular>(language)
                              : i18n::text<i18n::Key::CliBytePlural>(language));
  } else if (size < MIB) {
    fmt::format_to(std::back_inserter(out), "{:.1f} KiB", size / KIB);
  } else if (size < GIB) {
    fmt::format_to(std::back_inserter(out), "{:.1f} MiB", size / MIB);
  } else {
    fmt::format_to(std::back_inserter(out), "{:.1f} GiB", size / GIB);
  }
}

// A run's wall time covers the program's own execution, so a duration
// beside it would be reporting the program as the build. Every other
// outcome measures the compiler alone, and says so.
bool note_has_duration(const Envelope& envelope) {
  return envelope.outcome != Outcome::Ran;
}

bool note_has_size(const Envelope& envelope) {
  return envelope.outcome == Outcome::Built ||
         envelope.outcome == Outcome::Compiled;
}

// The subject of a result line: what was written, what was created, or
// what was counted.
std::string result_subject(const Envelope& envelope, i18n::Language language) {
  using i18n::Key;
  switch (envelope.outcome) {
    case Outcome::Failed: return {};
    case Outcome::CreatedPackage:
      return i18n::format<Key::CliPackageCreatedAt>(
          language, envelope.package_name, envelope.package_dir);
    case Outcome::Checked: {
      std::string subject;
      if (envelope.file_count > 0) {
        append_count(subject, envelope.file_count,
                     i18n::text<Key::CliFileSingular>(language),
                     i18n::text<Key::CliFilePlural>(language));
        subject += ", ";
      }
      if (envelope.module_count > 0) {
        append_count(subject, envelope.module_count,
                     i18n::text<Key::CliModuleSingular>(language),
                     i18n::text<Key::CliModulePlural>(language));
        subject += ", ";
      }
      if (envelope.function_count > 0) {
        append_count(subject, envelope.function_count,
                     i18n::text<Key::CliFunctionSingular>(language),
                     i18n::text<Key::CliFunctionPlural>(language));
      }
      return subject;
    }
    case Outcome::Built:
    case Outcome::Compiled:
    case Outcome::Ran: return envelope.output_path;
  }
  return {};
}

// The note after the subject: what was written, how long it took, and
// how much memory the run held at its peak. The peak is left out when
// the platform cannot measure it, which reads as "not measured" rather
// than as zero.
std::string result_note(const Envelope& envelope, i18n::Language language) {
  std::string note = "(";
  if (note_has_size(envelope) && envelope.output_bytes > 0) {
    append_size(note, envelope.output_bytes, language);
    note += ", ";
  }
  note += format_duration(envelope.wall_ns);
  if (note_has_duration(envelope) && envelope.peak_memory_bytes > 0) {
    std::string peak;
    append_size(peak, envelope.peak_memory_bytes, language);
    note += ", ";
    note += i18n::format<i18n::Key::CliMemoryPeak>(language, peak);
  }
  note += ')';
  return note;
}

// A verb and its subject, in the column every result line uses.
void render_labelled(std::string& out,
                     std::string_view verb,
                     std::string_view subject,
                     bool pad_verb,
                     bool color,
                     bool newline) {
  if (color) {
    append_bold_bright_green(out, verb);
  } else {
    out.append(verb);
  }
  if (!pad_verb) {
    // `Created package` is longer than the column and says something
    // else entirely, so it takes the column with it.
    if (!subject.empty()) {
      out.push_back(' ');
    }
  } else {
    // The subject starts at the same column on every line, so a session
    // reads as a table. A subject is never padded: a path's width is
    // unbounded and padding it would push the note off the screen.
    const usize width = verb.size();
    out.append(width < VERB_COLUMN ? VERB_COLUMN - width : 1, ' ');
  }
  if (subject.empty()) {
    if (newline) {
      out.push_back('\n');
    }
    return;
  }
  if (color) {
    append_underline(out, subject);
  } else {
    out.append(subject);
  }
  if (newline) {
    out.push_back('\n');
  }
}

// The verb, the subject, and the note, laid out in columns. The one
// place a result line is built, so the text report and the JSON
// `summary` cannot drift apart.
void render_result_line(std::string& out,
                        const Envelope& envelope,
                        bool color,
                        bool newline,
                        i18n::Language language) {
  const std::string_view verb = verb_for(envelope.outcome, language);
  const std::string subject = result_subject(envelope, language);
  render_labelled(out, verb, subject,
                  envelope.outcome != Outcome::CreatedPackage, color, false);

  if (note_has_size(envelope) || note_has_duration(envelope)) {
    out.append("  ");
    const std::string note = result_note(envelope, language);
    if (color) {
      append_dim(out, note);
    } else {
      out.append(note);
    }
  }
  if (newline) {
    out.push_back('\n');
  }
}

// The same line without colour and without its newline, which is what a
// JSON document carries: a reader wants the sentence, not the layout.
std::string result_line(const Envelope& envelope, i18n::Language language) {
  std::string line;
  render_result_line(line, envelope, false, false, language);
  return line;
}

}  // namespace

void announce(const Logger& err,
              std::string_view verb,
              std::string_view subject,
              const diag::RenderOptions& options) {
  std::string line;
  render_labelled(line, verb, subject, true, options.color, true);
  err.block(line);
}

u64 elapsed_ns_since(std::chrono::steady_clock::time_point start) {
  const std::chrono::steady_clock::duration elapsed =
      std::chrono::steady_clock::now() - start;
  return static_cast<u64>(elapsed.count());
}

void record_output(const std::string& output, Envelope& envelope) {
  envelope.output_path = output;
  const isize size = io::file_size(output);
  if (size > 0) {
    envelope.output_bytes = static_cast<u64>(size);
  }
}

// Composes the note that follows a bag whose arena was spent. It is
// written here rather than emitted into the bag, which has no room left
// by definition; the caller keeps the text alive while it renders.
std::string render_diagnostics(const Envelope& envelope,
                               const diag::RenderOptions& options) {
  std::string out;
  if (envelope.bag != nullptr && envelope.sources != nullptr) {
    envelope.bag->for_each([&](const diag::Diagnostic& diagnostic) {
      append_diagnostic_text(out, diagnostic, *envelope.sources, options);
    });
    const u32 dropped = envelope.bag->dropped_count();
    if (dropped > 0) {
      const std::string note =
          diag::dropped_diagnostics_note(dropped, options.language);
      append_diagnostic_text(out, diag::message(diag::Severity::Note, note),
                             options);
    }
  }
  if (envelope.failure.has_value()) {
    append_diagnostic_text(out, *envelope.failure, options);
  }
  return out;
}

std::string render_result(const Envelope& envelope,
                          const diag::RenderOptions& options) {
  std::string out;
  if (envelope.status == Status::Ok) {
    // A run already announced itself before the program started, and a
    // second line after the program's own output would sit below that
    // output and read as more of it. The exit code is the result, and
    // the caller has it.
    if (envelope.outcome != Outcome::Ran) {
      render_result_line(out, envelope, options.color, true, options.language);
    }
  }
  append_trace_text(out, envelope, options.color);
  return out;
}

void report(const Logger& out,
            const Logger& err,
            const Envelope& envelope,
            term::ColorMode color_mode,
            i18n::Language language,
            bool json) {
  if (json) {
    // One document, and it carries the diagnostics inside it, so a reader
    // has one thing to parse whatever the command did.
    out.block(render_json(envelope, language));
    return;
  }
  const diag::RenderOptions for_err{
      .color = term::console_color_style(term::Stream::Stderr, color_mode) !=
               term::ColorStyle::Off,
      .language = language,
  };
  const diag::RenderOptions for_out{
      .color = term::console_color_style(term::Stream::Stdout, color_mode) !=
               term::ColorStyle::Off,
      .language = language,
  };
  // Diagnostics first, so a reader watching a terminal reads the
  // complaints before the summary that follows them.
  const std::string diagnostics = render_diagnostics(envelope, for_err);
  if (!diagnostics.empty()) {
    err.block(diagnostics);
  }
  const std::string result = render_result(envelope, for_out);
  if (!result.empty()) {
    out.block(result);
  }
}

std::string render_json(const Envelope& envelope, i18n::Language language) {
  std::string out;
  out += R"({"version":1,"command":)";
  append_json_string(out, envelope.command);
  out += R"(,"status":)";
  append_json_string(out, envelope.status == Status::Ok ? "ok" : "error");
  // The outcome is the machine-readable half: a stable name a tool
  // matches on, rather than the sentence a human reads.
  out += R"(,"outcome":)";
  append_json_string(out, outcome_name(envelope.outcome));
  out += R"(,"summary":)";
  if (envelope.status != Status::Ok) {
    if (!envelope.failure.has_value()) {
      out += "null";
    } else {
      append_json_string(out, envelope.failure->message);
    }
  } else {
    append_json_string(out, result_line(envelope, language));
  }
  out += R"(,"output_path":)";
  if (envelope.output_path.empty()) {
    out += "null";
  } else {
    append_json_string(out, envelope.output_path);
  }
  // The statistics are flat and always present, so a reader indexes them
  // without first finding out which command ran.
  out += R"(,"stats":)";
  out += R"({"files":)";
  append_json_number(out, envelope.file_count);
  out += R"(,"modules":)";
  append_json_number(out, envelope.module_count);
  out += R"(,"functions":)";
  append_json_number(out, envelope.function_count);
  out += R"(,"output_bytes":)";
  append_json_number(out, envelope.output_bytes);
  out += R"(,"wall_ns":)";
  append_json_number(out, envelope.wall_ns);
  out += R"(,"peak_memory_bytes":)";
  append_json_number(out, envelope.peak_memory_bytes);
  out += '}';
  out += R"(,"diagnostics":)";
  // A reader of the document wants every error in one array, so a
  // failure the envelope carries counts as one more rather than as a
  // summary that no other failure has.
  diag::append_diagnostics_json(
      out, envelope.bag, envelope.sources, language,
      envelope.failure.has_value() ? &*envelope.failure : nullptr);
  // Only when there is something to carry: an empty array would say the
  // trace ran and recorded nothing, which is a different statement.
  if (!envelope.trace.events.empty()) {
    append_trace_json(out, envelope);
  }
  out += '}';
  out.push_back('\n');
  return out;
}

}  // namespace cli
