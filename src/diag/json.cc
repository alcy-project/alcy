// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/json.h"

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fmt/format.h"
#include "i18n/language.h"
#include "i18n/messages.h"
#include "source/source.h"
#include "text/json.h"

namespace diag {
namespace {

const char* severity_name(Severity severity) {
  switch (severity) {
    case Severity::Note: return "note";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
  }
  return "error";
}

// The component a code came from, in the spelling the module directory
// uses, so a tool reading `--json` sees the same name a source file does
// and never has to know the letter table.
const char* stage_name(Stage stage) {
  switch (stage) {
    case Stage::Lexer: return "lexer";
    case Stage::Parser: return "parser";
    case Stage::Analyzer: return "analyzer";
    case Stage::Lowering: return "lowering";
    case Stage::Borrow: return "borrow";
    case Stage::Ir: return "ir";
    case Stage::Pkg: return "pkg";
    case Stage::Pipeline: return "pipeline";
    case Stage::CodegenLlvm: return "codegen_llvm";
    case Stage::CodegenNative: return "codegen";
  }
  return "lexer";
}

// A span is only meaningful against a file the reader can reopen, so a
// diagnostic that never got one emits the field as null rather than as
// offsets into nothing.
void append_span(std::string& out,
                 const Span& span,
                 const source::SourceManager* sources) {
  const std::optional<std::string_view> name =
      sources != nullptr ? sources->name(span.file) : std::nullopt;
  if (!name.has_value()) {
    out += "null";
    return;
  }
  out += R"({"file":)";
  text::append_json_string(out, *name);
  out += R"(,"offset":)";
  text::append_json_number(out, span.offset);
  out += R"(,"length":)";
  text::append_json_number(out, span.length);
  out += '}';
}

}  // namespace

std::string dropped_diagnostics_note(u32 dropped, i18n::Language language) {
  fmt::memory_buffer text;
  if (dropped == 1) {
    i18n::format_to<i18n::Key::RenderDiagnosticsDroppedSingular>(
        std::back_inserter(text), language, dropped);
  } else {
    i18n::format_to<i18n::Key::RenderDiagnosticsDroppedPlural>(
        std::back_inserter(text), language, dropped);
  }
  return std::string(text.data(), text.size());
}

void append_diagnostic_json(std::string& out,
                            const Diagnostic& diagnostic,
                            const source::SourceManager* sources) {
  fmt::format_to(std::back_inserter(out),
                 R"json({{"severity":"{}","code":)json",
                 severity_name(diagnostic.severity));
  // A message from outside a check area carries no code, and null says so
  // where one would imply that exists. What a code *is* arrives as its
  // two parts rather than as a string to be parsed: a tool matches the
  // printed form, and reads the component and the id without having to
  // know the letter table.
  if (diagnostic.code.has_value()) {
    fmt::format_to(std::back_inserter(out),
                   R"json({{"stage":"{}","local_id":{}}})json",
                   stage_name(diagnostic.code->stage), diagnostic.code->id);
  } else {
    out += "null";
  }
  out += R"(,"message":)";
  text::append_json_string(out, diagnostic.message);
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
    text::append_json_string(out, diagnostic.labels[i].message);
    out += '}';
  }
  out += "]}";
}

void append_diagnostics_json(std::string& out,
                             const DiagBag* bag,
                             const source::SourceManager* sources,
                             i18n::Language language,
                             const Diagnostic* extra) {
  out += '[';
  u32 index = 0;
  const auto append_one = [&](const Diagnostic& diagnostic) {
    if (index > 0) {
      out += ',';
    }
    ++index;
    append_diagnostic_json(out, diagnostic, sources);
  };
  if (bag != nullptr) {
    bag->for_each(append_one);
    const u32 dropped = bag->dropped_count();
    if (dropped > 0) {
      append_one(
          message(Severity::Note, dropped_diagnostics_note(dropped, language)));
    }
  }
  if (extra != nullptr) {
    append_one(*extra);
  }
  out += ']';
}

}  // namespace diag
