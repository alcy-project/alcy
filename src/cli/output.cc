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

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "diag/span.h"
#include "fmt/core.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/profiler/profile_event.h"
#include "fpag/io/io_util.h"
#include "fpag/term/style.h"
#include "i18n/language.h"
#include "i18n/messages.h"
#include "source/source.h"
#include "text/json.h"

namespace cli {

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
  out.push_back('\n');
}

const char* severity_name(diag::Severity severity) {
  switch (severity) {
    case diag::Severity::Note: return "note";
    case diag::Severity::Warning: return "warning";
    case diag::Severity::Error: return "error";
  }
  return "error";
}

// Numbers are written as decimal digits with no locale and no quoting,
// which is all a JSON number is. Doing it here rather than through the
// formatter keeps the raw-string literals below free of brace escaping,
// where a `}` that is not a placeholder is easy to miscount.
template <typename T>
void append_json_number(std::string& out, T value) {
  fmt::format_to(std::back_inserter(out), "{}", value);
}

// A span is only meaningful against a file the reader can reopen, so a
// diagnostic that never got one emits the field as null rather than as
// offsets into nothing.
void append_span(std::string& out,
                 const diag::Span& span,
                 const source::SourceManager* sources) {
  const std::optional<std::string_view> name =
      sources != nullptr ? sources->name(span.file) : std::nullopt;
  if (!name.has_value()) {
    out += "null";
    return;
  }
  out += R"({"file":)";
  append_json_string(out, *name);
  out += R"(,"offset":)";
  append_json_number(out, span.offset);
  out += R"(,"length":)";
  append_json_number(out, span.length);
  out += '}';
}

void append_diagnostic_json(std::string& out,
                            const diag::Diagnostic& diagnostic,
                            const source::SourceManager* sources) {
  fmt::format_to(std::back_inserter(out),
                 R"json({{"severity":"{}","code":{},"message":)json",
                 severity_name(diagnostic.severity), diagnostic.code);
  append_json_string(out, diagnostic.message);
  out += R"(,"span":)";
  if (diagnostic.has_primary_span) {
    append_span(out, diagnostic.primary_span, sources);
  } else {
    out += "null";
  }
  if (diagnostic.label_count == 0) {
    out += '}';
    return;
  }
  out += R"(,"labels":[)";
  for (u32 i = 0; i < diagnostic.label_count; ++i) {
    if (i > 0) {
      out += ',';
    }
    out += R"({"span":)";
    append_span(out, diagnostic.labels[i].span, sources);
    out += R"(,"message":)";
    append_json_string(out, diagnostic.labels[i].message);
    out += '}';
  }
  out += "]}";
}

// One phase per name, summed. Names repeat when a phase runs more than
// once, and the reader wants the phase's cost rather than the two runs'
// identities.
struct PhaseTotal {
  const char* name = nullptr;
  const char* category = nullptr;
  u64 ns = 0;
  u32 count = 0;
};

std::vector<PhaseTotal> phase_totals(const Envelope& envelope) {
  std::vector<PhaseTotal> totals;
  for (const debug::ProfileEvent& event : envelope.trace) {
    if (event.name == nullptr) {
      continue;
    }
    const auto found = std::find_if(
        totals.begin(), totals.end(),
        [&](const PhaseTotal& total) { return total.name == event.name; });
    if (found == totals.end()) {
      totals.push_back(
          PhaseTotal{event.name, event.category, event.duration_ns, 1});
      continue;
    }
    found->ns += event.duration_ns;
    ++found->count;
  }
  // Longest first, then by name, so two runs of the same command produce
  // the same table in the same order.
  std::stable_sort(totals.begin(), totals.end(),
                   [](const PhaseTotal& a, const PhaseTotal& b) {
                     if (a.ns != b.ns) {
                       return a.ns > b.ns;
                     }
                     return std::string_view(a.name) < std::string_view(b.name);
                   });
  return totals;
}

void append_trace_json(std::string& out, const Envelope& envelope) {
  // A complete event carries a duration rather than a timestamp, and
  // the array sits at the top level because that is what a trace viewer
  // reads.
  out += R"(,"traceEvents":[)";
  for (usize i = 0; i < envelope.trace.size(); ++i) {
    const debug::ProfileEvent& event = envelope.trace[i];
    if (i > 0) {
      out += ',';
    }
    out += R"({"name":)";
    append_json_string(out, event.name != nullptr ? event.name : "");
    out += R"(,"cat":)";
    append_json_string(out, event.category != nullptr ? event.category : "");
    out += R"(,"ph":"X","pid":)";
    append_json_number(out, event.process_id);
    out += R"(,"tid":)";
    append_json_number(out, event.thread_id);
    // Microseconds, which is the unit the trace viewers assume for a
    // complete event, and the call site that recorded it.
    out += R"(,"ts":)";
    append_json_number(out, static_cast<double>(event.start_time_ns) / 1000.0);
    out += R"(,"dur":)";
    append_json_number(out, static_cast<double>(event.duration_ns) / 1000.0);
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

void append_trace_text(std::string& out,
                       const Envelope& envelope,
                       i18n::Language language) {
  if (envelope.trace.empty()) {
    return;
  }
  out += '\n';
  out += i18n::text<i18n::Key::CliPhaseTimings>(language);
  out += ":\n";
  for (const PhaseTotal& total : phase_totals(envelope)) {
    fmt::format_to(std::back_inserter(out), "  {:<14} {:>8.3} ms  {}\n",
                   total.name, static_cast<double>(total.ns) / 1.0e6,
                   total.category);
  }
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

void append_red(std::string& out, std::string_view text) {
  out += term::FG_RED;
  out.append(text);
  out += term::RESET;
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

void append_duration(std::string& out, u64 ns) {
  constexpr f64 MICRO = 1000.0;
  const f64 us = static_cast<f64>(ns) / MICRO;
  if (us < 1000.0) {
    fmt::format_to(std::back_inserter(out), "{:.0f} us", us);
  } else if (us < 1000000.0) {
    fmt::format_to(std::back_inserter(out), "{:.1f} ms", us / 1000.0);
  } else {
    fmt::format_to(std::back_inserter(out), "{:.2f} s", us / 1000000.0);
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

// The note after the subject: what was written, and how long it took.
std::string result_note(const Envelope& envelope, i18n::Language language) {
  std::string note = "(";
  if (note_has_size(envelope) && envelope.output_bytes > 0) {
    append_size(note, envelope.output_bytes, language);
    note += ", ";
  }
  append_duration(note, envelope.wall_ns);
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

void announce(std::string_view verb,
              std::string_view subject,
              const diag::RenderOptions& options) {
  std::string line;
  render_labelled(line, verb, subject, true, options.color, true);
  io::write(io::STDERR_FD, line.data(), line.size());
}

u64 elapsed_ns_since(std::chrono::steady_clock::time_point start) {
  const std::chrono::steady_clock::duration elapsed =
      std::chrono::steady_clock::now() - start;
  return static_cast<u64>(elapsed.count());
}

std::string render_text(const Envelope& envelope,
                        const diag::RenderOptions& options) {
  std::string out;
  if (envelope.bag != nullptr && envelope.sources != nullptr) {
    envelope.bag->for_each([&](const diag::Diagnostic& diagnostic) {
      append_diagnostic_text(out, diagnostic, *envelope.sources, options);
    });
  }
  if (envelope.status == Status::Ok) {
    // A run already announced itself before the program started, and a
    // second line after the program's own output would sit below that
    // output and read as more of it. The exit code is the result, and
    // the caller has it.
    if (envelope.outcome != Outcome::Ran) {
      render_result_line(out, envelope, options.color, true, options.language);
    }
  } else if (!envelope.failure.empty()) {
    // A failure with no diagnostic behind it. The message is the result,
    // and it gets the same colour an error would have.
    if (options.color) {
      append_red(out, envelope.failure);
    } else {
      out.append(envelope.failure);
    }
    out.push_back('\n');
  }
  append_trace_text(out, envelope, options.language);
  return out;
}

void report(const Envelope& envelope,
            const diag::RenderOptions& options,
            bool json) {
  const std::string text = json ? render_json(envelope, options.language)
                                : render_text(envelope, options);
  if (text.empty()) {
    return;
  }
  io::write(io::STDOUT_FD, text.data(), text.size());
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
    if (envelope.failure.empty()) {
      out += "null";
    } else {
      append_json_string(out, envelope.failure);
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
  out += '}';
  out += R"(,"diagnostics":[)";
  if (envelope.bag != nullptr) {
    u32 index = 0;
    envelope.bag->for_each([&](const diag::Diagnostic& diagnostic) {
      if (index > 0) {
        out += ',';
      }
      ++index;
      append_diagnostic_json(out, diagnostic, envelope.sources);
    });
  }
  out += ']';
  // Only when there is something to carry: an empty array would say the
  // trace ran and recorded nothing, which is a different statement.
  if (!envelope.trace.empty()) {
    append_trace_json(out, envelope);
  }
  out += '}';
  out.push_back('\n');
  return out;
}

}  // namespace cli
