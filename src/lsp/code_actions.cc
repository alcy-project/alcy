// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/code_actions.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "lsp/analysis.h"
#include "lsp/analysis_cache.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "lsp/workspace.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"
#include "text/json.h"

namespace lsp {

std::string_view code_action_kind_name(diag::FixKind kind) {
  switch (kind) {
    case diag::FixKind::Refactor: return CODE_ACTION_REFACTOR;
    case diag::FixKind::QuickFix: return CODE_ACTION_QUICKFIX;
  }
  return CODE_ACTION_QUICKFIX;
}

namespace {

constexpr std::string_view STD_MARKER = "(alcy/std/";

bool intersects(usize start, usize end, diag::Span span) {
  return span.offset < end && start < span.offset + span.length;
}

struct ManifestSnapshot {
  std::string uri;
  std::string text;
};

struct ManifestFix {
  usize offset = 0;
  std::string replacement;
};

std::optional<ManifestSnapshot> manifest_source(const Document& document,
                                                AnalysisCache& cache) {
  const std::optional<std::string> path =
      workspace::uri_to_path(document.uri());
  if (!path.has_value()) {
    return std::nullopt;
  }
  const std::optional<path::Path> root = workspace::find_package_root(*path);
  if (!root.has_value()) {
    return std::nullopt;
  }
  const path::Path manifest = root->join(pkg::MANIFEST_FILE_NAME);
  const std::string manifest_uri = workspace::path_to_uri(manifest.as_view());
  const Document* open = cache.open_document(manifest_uri);
  std::string text = open == nullptr
                         ? io::read_file(std::string(manifest.as_view()))
                         : std::string(open->text());
  if (text.empty()) {
    return std::nullopt;
  }
  return ManifestSnapshot{manifest_uri, std::move(text)};
}

std::optional<ManifestFix> std_dependency_fix(
    const diag::Diagnostic& diagnostic,
    const std::string& text) {
  const usize marker_at = diagnostic.message.find(STD_MARKER);
  if (marker_at == std::string_view::npos) {
    return std::nullopt;
  }
  const usize end = diagnostic.message.find(')', marker_at + STD_MARKER.size());
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view member = diagnostic.message.substr(
      marker_at + STD_MARKER.size(), end - marker_at - STD_MARKER.size());
  if (member.empty()) {
    return std::nullopt;
  }
  const std::string spelling = "\"alcy/std/" + std::string(member) + "\" = {}";
  if (text.find(spelling) != std::string::npos) {
    return std::nullopt;
  }
  constexpr std::string_view section = "[dependencies]";
  const usize section_at = text.find(section);
  usize offset = text.size();
  std::string replacement =
      "\n" + std::string(section) + "\n" + spelling + "\n";
  if (section_at != std::string::npos) {
    const usize line_end = text.find('\n', section_at);
    if (line_end == std::string::npos) {
      offset = text.size();
      replacement = "\n" + spelling + "\n";
    } else {
      offset = line_end + 1;
      replacement = spelling + "\n";
    }
  }
  return ManifestFix{offset, std::move(replacement)};
}

}  // namespace

text::JsonValue code_actions(const Document& document,
                             AnalysisCache& cache,
                             Position start,
                             Position end) {
  const usize start_offset =
      document.index().byte_offset(document.text(), start);
  const usize end_offset = document.index().byte_offset(document.text(), end);

  const DocumentAnalysis& analysis = cache.get(document);
  const diag::DiagBag& bag = analysis.owner().context.bag;
  text::JsonValue actions = text::JsonValue::make_array();
  std::vector<std::pair<usize, usize>> seen;
  for (u32 i = 0; i < bag.size(); ++i) {
    const diag::Diagnostic* diagnostic = bag.at(i);
    if (diagnostic == nullptr || diagnostic->fix == nullptr ||
        diagnostic->fix->span.file != analysis.root()) {
      continue;
    }
    const diag::Fix& fix = *diagnostic->fix;
    const bool in_range =
        (diagnostic->has_primary_span &&
         intersects(start_offset, end_offset, diagnostic->primary_span)) ||
        intersects(start_offset, end_offset, fix.span);
    if (!in_range) {
      continue;
    }
    bool duplicate = false;
    for (const std::pair<usize, usize>& key : seen) {
      if (key.first == fix.span.offset && key.second == fix.span.length) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    seen.emplace_back(fix.span.offset, fix.span.length);

    text::JsonValue edit = text::JsonValue::make_object();
    edit.set("range", range_json(document, fix.span.offset, fix.span.length));
    edit.set("newText",
             text::JsonValue::make_string(std::string(fix.replacement)));
    text::JsonValue edits = text::JsonValue::make_array();
    edits.push(std::move(edit));
    text::JsonValue changes = text::JsonValue::make_object();
    changes.set(std::string(document.uri()), std::move(edits));
    text::JsonValue workspace_edit = text::JsonValue::make_object();
    workspace_edit.set("changes", std::move(changes));

    text::JsonValue action = text::JsonValue::make_object();
    action.set("title",
               text::JsonValue::make_string(std::string(diagnostic->message)));
    action.set("kind", text::JsonValue::make_string(
                           std::string(code_action_kind_name(fix.kind))));
    action.set("edit", std::move(workspace_edit));
    actions.push(std::move(action));
  }
  std::optional<ManifestSnapshot> manifest;
  bool manifest_checked = false;
  std::unordered_set<std::string> emitted_dependencies;
  for (u32 i = 0; i < bag.size(); ++i) {
    const diag::Diagnostic* diagnostic = bag.at(i);
    if (diagnostic == nullptr || !diagnostic->has_primary_span ||
        diagnostic->primary_span.file != analysis.root() ||
        diagnostic->message.find(STD_MARKER) == std::string_view::npos) {
      continue;
    }
    if (!manifest_checked) {
      manifest = manifest_source(document, cache);
      manifest_checked = true;
    }
    if (!manifest.has_value()) {
      continue;
    }
    const std::optional<ManifestFix> fix =
        std_dependency_fix(*diagnostic, manifest->text);
    if (!fix.has_value()) {
      continue;
    }
    if (!intersects(start_offset, end_offset, diagnostic->primary_span)) {
      continue;
    }
    if (!emitted_dependencies.insert(fix->replacement).second) {
      continue;
    }
    PositionCache positions;
    text::JsonValue edit = text::JsonValue::make_object();
    edit.set("range", range_json(manifest->text, static_cast<u32>(fix->offset),
                                 0, analysis.owner().encoding, &positions));
    edit.set("newText", text::JsonValue::make_string(fix->replacement));
    text::JsonValue edits = text::JsonValue::make_array();
    edits.push(std::move(edit));
    text::JsonValue changes = text::JsonValue::make_object();
    changes.set(manifest->uri, std::move(edits));
    text::JsonValue workspace_edit = text::JsonValue::make_object();
    workspace_edit.set("changes", std::move(changes));
    text::JsonValue action = text::JsonValue::make_object();
    action.set("title",
               text::JsonValue::make_string(
                   "Add the standard library member to [dependencies]"));
    action.set("kind",
               text::JsonValue::make_string(std::string(CODE_ACTION_SOURCE)));
    action.set("edit", std::move(workspace_edit));
    actions.push(std::move(action));
  }
  return actions;
}

text::JsonValue code_actions(const Document& document,
                             Position start,
                             Position end) {
  AnalysisCache cache;
  return code_actions(document, cache, start, end);
}

}  // namespace lsp
