// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/will_rename.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "doctest/doctest.h"
#include "fpag/io/temp_dir.h"
#include "lsp/analysis_cache.h"
#include "lsp/document.h"
#include "lsp/workspace.h"
#include "text/json.h"

namespace lsp {

namespace {

constexpr std::string_view MANIFEST =
    "[package]\n"
    "name = \"app\"\n"
    "version = \"0.1.0\"\n"
    "license = \"\"\n"
    "\n"
    "[modules]\n"
    "include = [\"main\", \"helper\"]\n"
    "export = [\"helper\"]\n"
    "\n"
    "[[bin]]\n"
    "name = \"main\"\n"
    "path = \"main.al\"\n";
constexpr std::string_view MAIN =
    "use package::helper::Helper;\n"
    "fn main() -> i32 {\n"
    "  ret 0\n"
    "}\n";
constexpr std::string_view HELPER = "pub struct Helper {\n  x: i32,\n}\n";

bool has_edit(const text::JsonValue& changes,
              std::string_view uri,
              std::string_view new_text) {
  const text::JsonValue* edits = changes.find(uri);
  if (edits == nullptr || !edits->is_array()) {
    return false;
  }
  for (const text::JsonValue& edit : edits->as_array()) {
    const text::JsonValue* text = edit.find("newText");
    if (text != nullptr && text->is_string() && text->as_string() == new_text) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("Renaming a module rewrites its uses and the manifest") {
  io::TempDir dir = io::TempDir::create_unique("alcy_lsp_rename_");
  CHECK(dir.write_file("proj/alcy.toml", MANIFEST));
  CHECK(dir.write_file("proj/main.al", MAIN));
  CHECK(dir.write_file("proj/helper.al", HELPER));
  const std::string root = std::string(dir.join("proj"));
  const std::string main_uri = workspace::path_to_uri(root + "/main.al");
  const std::string helper_uri = workspace::path_to_uri(root + "/helper.al");
  const std::string utils_uri = workspace::path_to_uri(root + "/utils.al");
  const std::string manifest_uri = workspace::path_to_uri(root + "/alcy.toml");

  DocumentStore documents;
  documents.open(main_uri, "alcy", 1, std::string(MAIN));
  AnalysisCache cache;
  cache.set_watched_files(true);

  text::JsonValue file = text::JsonValue::make_object();
  file.set("oldUri", text::JsonValue::make_string(helper_uri));
  file.set("newUri", text::JsonValue::make_string(utils_uri));
  text::JsonValue files = text::JsonValue::make_array();
  files.push(std::move(file));
  text::JsonValue params = text::JsonValue::make_object();
  params.set("files", std::move(files));

  const std::optional<text::JsonValue> edit =
      will_rename_files(documents, cache, params);
  CHECK(edit.has_value());
  if (!edit.has_value()) {
    return;
  }
  const text::JsonValue* changes = edit->find("changes");
  CHECK(changes != nullptr);
  if (changes == nullptr) {
    return;
  }
  CHECK(has_edit(*changes, main_uri, "utils"));
  CHECK(has_edit(*changes, manifest_uri, "utils"));
  CHECK(changes->find(helper_uri) == nullptr);
}

TEST_CASE("A rename that names no module needs no edit") {
  DocumentStore documents;
  AnalysisCache cache;
  text::JsonValue file = text::JsonValue::make_object();
  file.set("oldUri", text::JsonValue::make_string("untitled:old"));
  file.set("newUri", text::JsonValue::make_string("untitled:new"));
  text::JsonValue files = text::JsonValue::make_array();
  files.push(std::move(file));
  text::JsonValue params = text::JsonValue::make_object();
  params.set("files", std::move(files));
  CHECK(!will_rename_files(documents, cache, params).has_value());
}

}  // namespace lsp
