// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/output.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "diag/span.h"
#include "fmt/format.h"
#include "fpag/io/io_util.h"
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

void append_trace_text(std::string& out, const Envelope& envelope) {
  if (envelope.trace.empty()) {
    return;
  }
  out += "\nphase timings (total, inclusive):\n";
  for (const PhaseTotal& total : phase_totals(envelope)) {
    fmt::format_to(std::back_inserter(out), "  {:<14} {:>8.3} ms  {}\n",
                   total.name, static_cast<double>(total.ns) / 1.0e6,
                   total.category);
  }
}

}  // namespace

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
  if (!envelope.summary.empty()) {
    out.append(envelope.summary);
    // The statistics are the answer to "what did it cost", so they sit
    // on the line that reports the result rather than on their own.
    if (envelope.file_count > 0 || envelope.module_count > 0 ||
        envelope.function_count > 0) {
      fmt::format_to(
          std::back_inserter(out), ": {} file(s), {} module(s), {} function(s)",
          envelope.file_count, envelope.module_count, envelope.function_count);
    }
    if (envelope.output_bytes > 0) {
      fmt::format_to(std::back_inserter(out), ": {} byte(s)",
                     envelope.output_bytes);
    }
    out.push_back('\n');
  }
  append_trace_text(out, envelope);
  return out;
}

void report(const Envelope& envelope,
            const diag::RenderOptions& options,
            bool json) {
  const std::string text =
      json ? render_json(envelope) : render_text(envelope, options);
  if (text.empty()) {
    return;
  }
  io::write(io::STDOUT_FD, text.data(), text.size());
}

std::string render_json(const Envelope& envelope) {
  std::string out;
  out += R"({"version":1,"command":)";
  append_json_string(out, envelope.command);
  out += R"(,"status":)";
  append_json_string(out, envelope.status == Status::Ok ? "ok" : "error");
  out += R"(,"summary":)";
  if (envelope.summary.empty()) {
    out += "null";
  } else {
    append_json_string(out, envelope.summary);
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
