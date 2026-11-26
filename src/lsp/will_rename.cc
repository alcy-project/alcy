// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/will_rename.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "lsp/analysis.h"
#include "lsp/analysis_cache.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "lsp/workspace.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

struct Edit {
  usize offset = 0;
  usize length = 0;
  std::string text;
};

std::string_view field_string(const text::JsonValue& object,
                              std::string_view key) {
  const text::JsonValue* value = object.find(key);
  if (value == nullptr || !value->is_string()) {
    return {};
  }
  return value->as_string();
}

std::string module_name(std::string_view relative, std::string_view joiner) {
  std::string result;
  for (const char c : relative) {
    if (c == path::DEFAULT_PATH_SEPARATOR || c == '\\') {
      result += joiner;
    } else {
      result.push_back(c);
    }
  }
  return result;
}

std::optional<std::string> module_from_path(std::string_view root,
                                            std::string_view file,
                                            std::string_view joiner) {
  if (!workspace::under_root(file, root) || !file.starts_with(root)) {
    return std::nullopt;
  }
  std::string_view relative = file.substr(root.size());
  while (!relative.empty() &&
         (relative.front() == path::DEFAULT_PATH_SEPARATOR ||
          relative.front() == '\\')) {
    relative.remove_prefix(1);
  }
  if (!path::has_source_extension(relative)) {
    return std::nullopt;
  }
  relative.remove_suffix(path::SOURCE_EXTENSION.size());
  if (relative.empty()) {
    return std::nullopt;
  }
  return module_name(relative, joiner);
}

std::vector<std::string_view> split(std::string_view value,
                                    std::string_view separator) {
  std::vector<std::string_view> parts;
  usize begin = 0;
  while (begin <= value.size()) {
    const usize end = value.find(separator, begin);
    if (end == std::string_view::npos) {
      parts.push_back(value.substr(begin));
      break;
    }
    parts.push_back(value.substr(begin, end - begin));
    begin = end + separator.size();
  }
  return parts;
}

// One path spells the old module when its segments contain the whole
// old module path in order. Replacing that range handles a module that
// moved to a different depth without touching a same-named local.
void collect_path_edits(const ast::AstArena& ast,
                        source::FileId file,
                        const std::vector<std::string_view>& old_segments,
                        std::string_view replacement,
                        std::vector<Edit>* out) {
  ast.paths.for_each_node([&](ast::PathIdx index) {
    const ast::Path& path = ast.paths[index];
    if (path.span.file != file || path.segments.size() < old_segments.size()) {
      return;
    }
    for (usize at = 0; at + old_segments.size() <= path.segments.size(); ++at) {
      bool matched = true;
      for (usize part = 0; part < old_segments.size(); ++part) {
        if (path.segments[at + part].name != old_segments[part]) {
          matched = false;
          break;
        }
      }
      if (!matched) {
        continue;
      }
      const diag::Span first = path.segments[at].span;
      const diag::Span last = path.segments[at + old_segments.size() - 1].span;
      out->push_back(Edit{first.offset,
                          last.offset + last.length - first.offset,
                          std::string(replacement)});
    }
  });
}

// Only the module list is a declaration of module names. Other quoted
// strings in the manifest name packages, specs or targets.
void collect_manifest_edits(std::string_view text,
                            std::string_view old_name,
                            std::string_view new_name,
                            std::vector<Edit>* out) {
  const usize section = text.find("[modules]");
  if (section == std::string_view::npos) {
    return;
  }
  usize end = text.size();
  for (usize at = section + 1; at < text.size(); ++at) {
    if (text[at] == '[' && (at == 0 || text[at - 1] == '\n')) {
      end = at;
      break;
    }
  }
  for (usize at = section; at < end;) {
    if (text[at] != '"') {
      ++at;
      continue;
    }
    const usize value = at + 1;
    const usize close = text.find('"', value);
    if (close == std::string_view::npos || close >= end) {
      break;
    }
    if (text.substr(value, close - value) == old_name) {
      out->push_back(Edit{value, close - value, std::string(new_name)});
    }
    at = close + 1;
  }
}

std::optional<source::FileId> manifest_file(
    const pipeline::PipelineContext& context,
    std::string_view root) {
  for (u32 id = 0; id < context.sources.file_count(); ++id) {
    const std::optional<std::string_view> name = context.sources.name(id);
    if (!name.has_value()) {
      continue;
    }
    const usize slash = name->find_last_of(path::DEFAULT_PATH_SEPARATOR);
    const std::string_view base =
        slash == std::string_view::npos ? *name : name->substr(slash + 1);
    if (base == pkg::MANIFEST_FILE_NAME && workspace::under_root(*name, root)) {
      return static_cast<source::FileId>(id);
    }
  }
  return std::nullopt;
}

text::JsonValue edits_json(const std::string_view source,
                           std::vector<Edit> edits,
                           PositionEncoding encoding) {
  std::sort(edits.begin(), edits.end(),
            [](const Edit& left, const Edit& right) {
              if (left.offset != right.offset) {
                return left.offset < right.offset;
              }
              return left.length < right.length;
            });
  edits.erase(std::unique(edits.begin(), edits.end(),
                          [](const Edit& left, const Edit& right) {
                            return left.offset == right.offset &&
                                   left.length == right.length &&
                                   left.text == right.text;
                          }),
              edits.end());
  PositionCache positions;
  text::JsonValue array = text::JsonValue::make_array();
  for (const Edit& edit : edits) {
    text::JsonValue item = text::JsonValue::make_object();
    item.set("range", range_json(source, edit.offset, edit.length, encoding,
                                 &positions));
    item.set("newText", text::JsonValue::make_string(edit.text));
    array.push(std::move(item));
  }
  return array;
}

}  // namespace

std::optional<text::JsonValue> will_rename_files(
    const DocumentStore& documents,
    AnalysisCache& cache,
    const text::JsonValue& params) {
  const text::JsonValue* files = params.find("files");
  if (files == nullptr || !files->is_array()) {
    return std::nullopt;
  }
  struct Target {
    std::string source;
    std::vector<Edit> edits;
  };
  std::unordered_map<std::string, Target> by_uri;
  bool any = false;

  for (const text::JsonValue& file : files->as_array()) {
    const std::optional<std::string> old_path =
        workspace::uri_to_path(field_string(file, "oldUri"));
    const std::optional<std::string> new_path =
        workspace::uri_to_path(field_string(file, "newUri"));
    if (!old_path.has_value() || !new_path.has_value()) {
      continue;
    }
    const std::optional<path::Path> root =
        workspace::find_package_root(*old_path);
    if (!root.has_value()) {
      continue;
    }
    const std::string root_path(root->as_view());
    const std::optional<std::string> old_module =
        module_from_path(root_path, *old_path, "::");
    const std::optional<std::string> new_module =
        module_from_path(root_path, *new_path, "::");
    if (!old_module.has_value() || !new_module.has_value() ||
        old_module == new_module) {
      continue;
    }

    const Document* focus = nullptr;
    for (const auto& [uri, document] : documents.documents()) {
      (void)uri;
      const std::optional<std::string> path =
          workspace::uri_to_path(document.uri());
      if (path.has_value() && workspace::under_root(*path, root_path)) {
        focus = &document;
        break;
      }
    }
    if (focus == nullptr) {
      continue;
    }
    const DocumentAnalysis& analysis = cache.analysis_for(*focus);
    if (!analysis.ok()) {
      continue;
    }
    const pipeline::PipelineContext& context = analysis.context();
    const std::vector<std::string_view> segments = split(*old_module, "::");

    for (const analyzer::ModuleNode* module : analysis.package().tree.modules) {
      if (module->file == source::UNKNOWN_FILE ||
          !context.sources.name(module->file).has_value()) {
        continue;
      }
      const std::string_view name = *context.sources.name(module->file);
      if (!workspace::under_root(name, root_path)) {
        continue;
      }
      const std::optional<std::string_view> text =
          context.sources.bytes(module->file);
      if (!text.has_value()) {
        continue;
      }
      std::vector<Edit> edits;
      collect_path_edits(context.ast, module->file, segments, *new_module,
                         &edits);
      if (edits.empty()) {
        continue;
      }
      const std::optional<std::string> uri =
          uri_of_file(analysis, module->file);
      if (!uri.has_value()) {
        continue;
      }
      auto& target = by_uri[*uri];
      if (target.source.empty()) {
        target.source = std::string(*text);
      }
      target.edits.insert(target.edits.end(), edits.begin(), edits.end());
      any = true;
    }

    const std::optional<source::FileId> manifest =
        manifest_file(context, root_path);
    if (manifest.has_value()) {
      const std::optional<std::string_view> text =
          context.sources.bytes(*manifest);
      const std::optional<std::string> uri = uri_of_file(analysis, *manifest);
      if (text.has_value() && uri.has_value()) {
        const std::optional<std::string> old_in_manifest =
            module_from_path(root_path, *old_path, "/");
        const std::optional<std::string> new_in_manifest =
            module_from_path(root_path, *new_path, "/");
        if (!old_in_manifest.has_value() || !new_in_manifest.has_value()) {
          continue;
        }
        std::vector<Edit> edits;
        collect_manifest_edits(*text, *old_in_manifest, *new_in_manifest,
                               &edits);
        if (!edits.empty()) {
          auto& target = by_uri[*uri];
          if (target.source.empty()) {
            target.source = std::string(*text);
          }
          target.edits.insert(target.edits.end(), edits.begin(), edits.end());
          any = true;
        }
      }
    }
  }

  if (!any) {
    return std::nullopt;
  }
  text::JsonValue changes = text::JsonValue::make_object();
  for (auto& [uri, target] : by_uri) {
    changes.set(uri, edits_json(target.source, std::move(target.edits),
                                PositionEncoding::Utf16));
  }
  text::JsonValue result = text::JsonValue::make_object();
  result.set("changes", std::move(changes));
  return result;
}

}  // namespace lsp
