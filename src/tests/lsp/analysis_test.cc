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
#include "doctest/doctest.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "lsp/analysis_cache.h"
#include "lsp/diagnostics.h"
#include "lsp/document.h"
#include "lsp/workspace.h"
#include "path/path.h"
#include "text/json.h"

namespace lsp {
namespace {

constexpr std::string_view MANIFEST =
    "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
    "[modules]\ninclude = [\"main\", \"helper\"]\n\n"
    "[[bin]]\npath = \"main.al\"\n";

constexpr std::string_view MAIN =
    "use package::helper::Helper;\n"
    "\n"
    "fn use_it(value: Helper) -> i32 {\n"
    "  ret 0\n"
    "}\n"
    "\n"
    "fn main() -> i32 {\n"
    "  ret 0\n"
    "}\n";

constexpr std::string_view HELPER =
    "pub struct Helper {\n"
    "  pub x: i32,\n"
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

TEST_CASE("A buffer without a manifest keeps the single-file reading") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  ret 0\n}\n");
  const DocumentAnalysis analysis(document);
  CHECK(analysis.ok());
  CHECK(analysis.owner().root.as_view() == ".");
}

}  // namespace lsp
