// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/analysis.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "doctest/doctest.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "lsp/analysis_cache.h"
#include "lsp/call_hierarchy.h"
#include "lsp/code_actions.h"
#include "lsp/code_lens.h"
#include "lsp/completion.h"
#include "lsp/diagnostics.h"
#include "lsp/document.h"
#include "lsp/highlights.h"
#include "lsp/navigation.h"
#include "lsp/rename.h"
#include "lsp/workspace.h"
#include "lsp/workspace_symbols.h"
#include "path/path.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {
namespace {

constexpr std::string_view MANIFEST =
    "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
    "[modules]\ninclude = [\"main\", \"helper\"]\n\n"
    "[[bin]]\npath = \"main.al\"\n";

constexpr std::string_view MAIN =
    "use package::helper::Helper;\n"
    "use package::helper::helper_fn;\n"
    "\n"
    "fn use_it(value: Helper) -> i32 {\n"
    "  ret 0\n"
    "}\n"
    "\n"
    "fn main() -> i32 {\n"
    "  ret helper_fn()\n"
    "}\n";

constexpr std::string_view HELPER =
    "pub struct Helper {\n"
    "  pub x: i32,\n"
    "}\n"
    "\n"
    "pub fn helper_fn() -> i32 {\n"
    "  ret 0\n"
    "}\n"
    "\n"
    "fn hidden_from_main() -> i32 {\n"
    "  ret 0\n"
    "}\n";

struct Fixture {
  io::TempDir dir = io::TempDir::create_unique("alcy_lsp_analysis_");
  std::string root;
  std::string main_uri;
  std::string helper_uri;

  Fixture() {
    CHECK(dir.write_file("proj/alcy.toml", MANIFEST));
    CHECK(dir.write_file("proj/main.al", MAIN));
    CHECK(dir.write_file("proj/helper.al", HELPER));
    root = std::string(dir.join("proj"));
    main_uri = workspace::path_to_uri(root + "/main.al");
    helper_uri = workspace::path_to_uri(root + "/helper.al");
  }

  std::vector<workspace::BufferView> buffers(
      std::string_view helper_text = HELPER) const {
    return {{main_uri, MAIN, 1}, {helper_uri, helper_text, 1}};
  }
};

bool has_message(const diag::DiagBag& bag, std::string_view needle) {
  for (u32 i = 0; i < bag.size(); ++i) {
    const diag::Diagnostic* diagnostic = bag.at(i);
    if (diagnostic != nullptr &&
        diagnostic->message.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("An import of a sibling module resolves from a package buffer") {
  const Fixture f;
  const std::vector<workspace::BufferView> buffers = f.buffers();
  auto analysis = build_analysis(buffers, f.main_uri, i18n::Language::EnUs);

  CHECK(analysis->ok);
  CHECK(!has_message(analysis->context.bag, "Unresolved import"));
  CHECK(analysis->checked.size() == 1);
  if (!analysis->ok || analysis->checked.empty()) {
    return;
  }
  const analyzer::CheckedPackage& package = analysis->checked[0];
  const auto main_file = analysis->files_by_uri.find(f.main_uri);
  const auto helper_file = analysis->files_by_uri.find(f.helper_uri);
  if (main_file == analysis->files_by_uri.end() ||
      helper_file == analysis->files_by_uri.end()) {
    return;
  }

  bool found_helper = false;
  for (const analyzer::ModuleNode* module : package.tree.modules) {
    for (const analyzer::Import& import : module->imports) {
      if (import.name != "Helper") {
        continue;
      }
      found_helper = true;
      CHECK_EQ(module->file, main_file->second);
      CHECK_EQ(package.tree.modules[import.target_module]->file,
               helper_file->second);
    }
  }
  CHECK(found_helper);
}

TEST_CASE("Editing a sibling buffer invalidates the package analysis") {
  Fixture f;
  DocumentStore documents;
  documents.open(f.main_uri, "alcy", 1, std::string(MAIN));
  documents.open(f.helper_uri, "alcy", 1, std::string(HELPER));
  AnalysisCache cache(documents);

  const Document* main = documents.find(f.main_uri);
  if (main == nullptr) {
    return;
  }
  const PackageAnalysis* first = &cache.owner_of(*main);
  CHECK(!has_message(first->context.bag, "Unresolved import"));

  documents.open(f.helper_uri, "alcy", 2,
                 "pub struct Renamed {\n  pub x: i32,\n}\n");
  main = documents.find(f.main_uri);
  if (main == nullptr) {
    return;
  }
  const PackageAnalysis* second = &cache.owner_of(*main);
  CHECK(second != first);
  CHECK(has_message(second->context.bag, "Unresolved import"));
}

TEST_CASE("Workspace URIs round-trip through paths with escapes") {
  CHECK(workspace::uri_to_path("file:///tmp/project/main.al") ==
        std::optional<std::string>("/tmp/project/main.al"));
  CHECK(workspace::uri_to_path("file:///tmp/a%20b/main.al") ==
        std::optional<std::string>("/tmp/a b/main.al"));
  CHECK(workspace::uri_to_path("untitled:Untitled-1").has_value() == false);
  CHECK(workspace::path_to_uri("/tmp/a b/main.al") ==
        "file:///tmp/a%20b/main.al");
  CHECK(workspace::path_to_uri("C:/src/main.al") == "file:///C:/src/main.al");
}

TEST_CASE("A source path finds the nearest package manifest") {
  const Fixture f;
  const std::optional<path::Path> root =
      workspace::find_package_root(f.root + "/main.al");
  CHECK(root.has_value());
  if (root.has_value()) {
    CHECK(root->as_view() == f.root);
  }
}

TEST_CASE("A pull with a previous result id follows the package fingerprint") {
  Fixture f;
  DocumentStore documents;
  documents.open(f.main_uri, "alcy", 1, std::string(MAIN));
  documents.open(f.helper_uri, "alcy", 1, std::string(HELPER));
  AnalysisCache analysis_cache(documents);
  DiagnosticsCache pull_cache;

  const Document* main = documents.find(f.main_uri);
  if (main == nullptr) {
    return;
  }
  const text::JsonValue first = pull_cache.pull(*main, analysis_cache, "");
  const text::JsonValue* id = first.find("resultId");
  if (id == nullptr) {
    return;
  }
  CHECK(first.find("kind")->as_string() == "full");

  documents.open(f.helper_uri, "alcy", 2,
                 "pub struct Renamed {\n  pub x: i32,\n}\n");
  main = documents.find(f.main_uri);
  if (main == nullptr) {
    return;
  }
  const text::JsonValue second =
      pull_cache.pull(*main, analysis_cache, id->as_string());
  CHECK(second.find("kind")->as_string() == "full");
}

TEST_CASE("Highlights do not paint a sibling definition into this buffer") {
  const Fixture f;
  const Document document(f.main_uri, "alcy", 1, std::string(MAIN));
  const DocumentAnalysis analysis(document);
  if (!analysis.ok()) {
    return;
  }
  const text::JsonValue highlights =
      document_highlights(document, analysis, Position{8, 8});
  CHECK(highlights.as_array().size() == 1);
}

TEST_CASE("Rename edits both the importer and the defining module") {
  const Fixture f;
  const Document document(f.main_uri, "alcy", 1, std::string(MAIN));
  const DocumentAnalysis analysis(document);
  if (!analysis.ok()) {
    return;
  }
  const std::optional<text::JsonValue> result =
      workspace_rename(document, analysis, Position{8, 8}, "renamed");
  if (!result.has_value()) {
    return;
  }
  const text::JsonValue* changes = result->find("changes");
  if (changes == nullptr) {
    return;
  }
  CHECK(changes->find(f.main_uri) != nullptr);
  CHECK(changes->find(f.helper_uri) != nullptr);
}

TEST_CASE("Completion offers only names visible from the buffer") {
  const Fixture f;
  const Document document(f.main_uri, "alcy", 1, std::string(MAIN));
  const DocumentAnalysis analysis(document);
  if (!analysis.ok()) {
    return;
  }
  const text::JsonValue list =
      completion_items(document, analysis, Position{0, 0});
  const text::JsonValue* items = list.find("items");
  if (items == nullptr) {
    return;
  }
  bool imported = false;
  for (const text::JsonValue& item : items->as_array()) {
    const text::JsonValue* label = item.find("label");
    if (label == nullptr) {
      continue;
    }
    if (label->as_string() == "helper_fn") {
      imported = true;
    }
    CHECK(label->as_string() != "hidden_from_main");
  }
  CHECK(imported);
}

TEST_CASE("An unresolved standard library name offers a manifest fix") {
  Fixture f;
  constexpr std::string_view source =
      "fn main() -> i32 {\n"
      "  ret 0\n"
      "}\n"
      "fn take(s: String) -> i32 {\n"
      "  ret 0\n"
      "}\n";
  CHECK(f.dir.write_file("proj/main.al", source));
  const Document document(f.main_uri, "alcy", 1, std::string(source));
  AnalysisCache cache;
  CHECK(cache.get(document).ok());

  const text::JsonValue actions =
      code_actions(document, cache, Position{0, 0}, Position{4, 1});
  bool manifest_fix = false;
  for (const text::JsonValue& action : actions.as_array()) {
    const text::JsonValue* edit = action.find("edit");
    if (edit == nullptr) {
      continue;
    }
    const text::JsonValue* changes = edit->find("changes");
    if (changes != nullptr && changes->find(workspace::path_to_uri(
                                  f.root + "/alcy.toml")) != nullptr) {
      manifest_fix = true;
    }
  }
  CHECK(manifest_fix);
}

TEST_CASE("Workspace symbols search files the client has not opened") {
  const Fixture f;
  DocumentStore documents;
  documents.open(f.main_uri, "alcy", 1, std::string(MAIN));
  AnalysisCache cache(documents);

  const text::JsonValue symbols =
      workspace_symbols(documents, cache, "helper_fn");
  bool found = false;
  for (const text::JsonValue& symbol : symbols.as_array()) {
    const text::JsonValue* location = symbol.find("location");
    if (location == nullptr) {
      continue;
    }
    const text::JsonValue* uri = location->find("uri");
    if (uri != nullptr && uri->as_string() == f.helper_uri) {
      found = true;
    }
  }
  CHECK(found);
}

TEST_CASE("Cross-file references survive equal spans in two files") {
  Fixture f;
  std::string main_source =
      "pub fn main_fn() -> i32 { ret 0 }\n"
      "fn m() -> i32 { ret main_fn() }\n";
  std::string helper_source =
      "use package::main::main_fn;\n"
      "fn h() -> i32 { ret main_fn() }\n";
  const usize main_at = main_source.rfind("main_fn");
  const usize helper_at = helper_source.rfind("main_fn");
  if (main_at > helper_at) {
    helper_source.insert(0, main_at - helper_at, ' ');
  } else {
    main_source.insert(0, helper_at - main_at, ' ');
  }
  CHECK(f.dir.write_file("proj/main.al", main_source));
  CHECK(f.dir.write_file("proj/helper.al", helper_source));

  const Document document(f.main_uri, "alcy", 1, main_source);
  const DocumentAnalysis analysis(document);
  if (!analysis.ok()) {
    return;
  }
  const usize offset = main_source.rfind("main_fn");
  const Position position = document.index().position_at(main_source, offset);
  const std::vector<diag::Span> sites =
      references_at(document, analysis, position);
  CHECK(sites.size() == 2);
  if (sites.size() == 2) {
    CHECK(sites[0].file != sites[1].file);
  }
}

TEST_CASE("Code lens locations point at the file that holds the reference") {
  const Fixture f;
  const Document document(f.main_uri, "alcy", 1, std::string(MAIN));
  const DocumentAnalysis analysis(document);
  if (!analysis.ok()) {
    return;
  }
  const text::JsonValue lenses = code_lenses(document, analysis);
  for (const text::JsonValue& lens : lenses.as_array()) {
    const text::JsonValue* command = lens.find("command");
    if (command == nullptr) {
      continue;
    }
    const text::JsonValue* arguments = command->find("arguments");
    if (arguments == nullptr || arguments->as_array().size() < 3) {
      continue;
    }
    const text::JsonValue& locations = arguments->as_array()[2];
    for (const text::JsonValue& location : locations.as_array()) {
      const text::JsonValue* uri = location.find("uri");
      if (uri == nullptr) {
        continue;
      }
      const bool known =
          uri->as_string() == f.main_uri || uri->as_string() == f.helper_uri;
      CHECK(known);
    }
    return;
  }
}

TEST_CASE("Completion does not duplicate a name from an unimported module") {
  Fixture f;
  constexpr std::string_view manifest =
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"main\", \"helper\", \"other\"]\n\n"
      "[[bin]]\npath = \"main.al\"\n";
  CHECK(f.dir.write_file("proj/alcy.toml", manifest));
  CHECK(f.dir.write_file("proj/other.al",
                         "pub fn helper_fn() -> i32 { ret 0 }\n"));
  const Document document(f.main_uri, "alcy", 1, std::string(MAIN));
  const DocumentAnalysis analysis(document);
  if (!analysis.ok()) {
    return;
  }
  const text::JsonValue list =
      completion_items(document, analysis, Position{0, 0});
  const text::JsonValue* items = list.find("items");
  if (items == nullptr) {
    return;
  }
  u32 count = 0;
  for (const text::JsonValue& item : items->as_array()) {
    const text::JsonValue* label = item.find("label");
    if (label != nullptr && label->as_string() == "helper_fn") {
      ++count;
    }
  }
  CHECK(count == 1);
}

TEST_CASE("Incoming calls resolve for a declaration in an unopened file") {
  const Fixture f;
  const Document document(f.main_uri, "alcy", 1, std::string(MAIN));
  const DocumentAnalysis analysis(document);
  if (!analysis.ok()) {
    return;
  }
  const text::JsonValue items =
      prepare_call_hierarchy(document, analysis, Position{8, 8});
  CHECK_FALSE(items.as_array().empty());
  if (items.as_array().empty()) {
    return;
  }
  const std::optional<source::FileId> item_file =
      file_of_uri(analysis, f.helper_uri);
  if (!item_file.has_value()) {
    return;
  }
  const text::JsonValue calls =
      incoming_calls(document, analysis, items.as_array().front(), *item_file);
  CHECK_FALSE(calls.as_array().empty());
}

TEST_CASE("A buffer without a manifest keeps the single-file reading") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  ret 0\n}\n");
  const DocumentAnalysis analysis(document);
  CHECK(analysis.ok());
  CHECK(analysis.owner().root.as_view() == ".");
}

}  // namespace lsp
