// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <algorithm>
#include <string>
#include <vector>

#if FPAG_BUILD_FLAG(IS_OS_WIN)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "doctest/doctest.h"
#include "fpag/build/build_flag.h"
#include "fpag/io/io_util.h"
#include "ssg/catalog.h"
#include "ssg/documents.h"
#include "ssg/markdown.h"
#include "ssg/paths.h"
#include "ssg/render.h"
#include "ssg/urls.h"

namespace ssg_test {

// The catalog the tests read: the one the site actually ships,
// loaded once from the repository. Tests fail loudly when it is
// missing instead of running against a made-up one. The walk starts
// at the working directory and climbs, because the runner's working
// directory is the build directory under out/, not the repository
// root.
inline std::string repo_root() {
  // The runner starts in the build directory under out/, so the walk
  // climbs until the site's catalog is a regular file above it.
  char buffer[4096];
  std::string cwd;
#if FPAG_BUILD_FLAG(IS_OS_WIN)
  const DWORD length = GetCurrentDirectoryA(sizeof(buffer), buffer);
  cwd = length > 0 && length < sizeof(buffer) ? std::string(buffer, length)
                                              : std::string();
  // The posix path helpers only split on '/', so a native Windows cwd
  // would look like one segment and the walk would stop before the
  // repository root. Windows accepts '/' in every path these helpers
  // build, including the one io::is_file opens.
  std::replace(cwd.begin(), cwd.end(), '\\', '/');
#else
  cwd = getcwd(buffer, sizeof(buffer)) != nullptr ? std::string(buffer)
                                                  : std::string();
#endif
  for (i32 depth = 0; depth < 8; depth++) {
    if (io::is_file(ssg::join_posix({cwd, "site/shared/catalog.json"}))) {
      return cwd;
    }
    const std::string parent = ssg::dirname_posix(cwd);
    if (parent == cwd || parent == ".") {
      break;
    }
    cwd = parent;
  }
  return {};
}

inline std::string site_shared_path() {
  const std::string root = repo_root();
  return root.empty() ? std::string()
                      : ssg::join_posix({root, "site", "shared"});
}

inline const ssg::Catalog& test_catalog() {
  static ssg::Catalog catalog = []() {
    const std::string path =
        ssg::join_posix({site_shared_path(), "catalog.json"});
    CHECK(!path.empty());
    auto loaded = ssg::load_catalog(path);
    CHECK(loaded.is_ok());
    return std::move(loaded).unwrap();
  }();
  return catalog;
}
inline ssg::DocPage test_page(std::string_view title,
                              std::string_view path,
                              std::string_view language = "en") {
  ssg::DocPage page;
  page.source_rel = std::string(title);
  page.path = std::string(path);
  page.order = 0;
  page.title = std::string(title);
  page.language = std::string(language);
  page.route =
      (language == "en" ? std::string() : std::string(language) + "/") +
      std::string(path);
  return page;
}

inline ssg::Block heading_block(i32 level, std::string_view text, usize line) {
  ssg::Block block;
  block.type = ssg::BlockType::Heading;
  block.line = line;
  block.level = level;
  block.text = std::string(text);
  return block;
}

inline ssg::Block paragraph_block(std::string_view text, usize line) {
  ssg::Block block;
  block.type = ssg::BlockType::Paragraph;
  block.line = line;
  block.text = std::string(text);
  return block;
}

inline ssg::Block code_block(std::string_view language,
                             std::string_view text,
                             usize line) {
  ssg::Block block;
  block.type = ssg::BlockType::Code;
  block.line = line;
  block.language = std::string(language);
  block.text = std::string(text);
  return block;
}

inline ssg::Block list_block(std::vector<std::string> items,
                             bool ordered,
                             usize line) {
  ssg::Block block;
  block.type = ssg::BlockType::List;
  block.line = line;
  block.items = std::move(items);
  block.ordered = ordered;
  return block;
}

inline ssg::Block table_block(std::vector<std::vector<std::string>> rows,
                              std::vector<ssg::CellAlign> aligns,
                              usize line) {
  ssg::Block block;
  block.type = ssg::BlockType::Table;
  block.line = line;
  block.rows = std::move(rows);
  block.aligns = std::move(aligns);
  return block;
}

inline ssg::Block prose_block() {
  ssg::Block block;
  block.type = ssg::BlockType::Paragraph;
  block.line = 1;
  block.text = "hello";
  return block;
}

inline ssg::Block shell_block() {
  ssg::Block block;
  block.type = ssg::BlockType::Code;
  block.line = 1;
  block.language = "sh";
  block.text = "echo hi";
  return block;
}

inline ssg::Block alcy_block() {
  ssg::Block block;
  block.type = ssg::BlockType::Code;
  block.line = 1;
  block.language = "alcy";
  block.text = "fn main() {}";
  return block;
}

// Renders one article page with the defaults the tests assert
// against: the catalog the site ships, an inline boot, a stamp the
// tests can match, and the fence untouched by the compiler.
inline std::string render_article(const std::vector<ssg::DocPage>& pages,
                                  const ssg::Tree& tree = ssg::GUIDE_TREE,
                                  std::string_view language = "en",
                                  std::vector<ssg::Block> blocks = {
                                      prose_block()}) {
  ssg::DocPage current =
      pages.empty() ? test_page("Title", "guide/x.html", language) : pages[0];
  current.blocks = std::move(blocks);
  ssg::PageOptions options;
  options.catalog = &test_catalog();
  options.pages = &pages;
  options.tree = &tree;
  options.language = language;
  options.boot = "INLINE_BOOT";
  options.stamp = {.text = "TESTSTAMP",
                   .url = "https://example.test/commit/TESTSTAMP"};
  options.highlight = [](std::string_view code) {
    return ssg::escape_html(code);
  };
  return ssg::render_guide_page(current, options);
}

}  // namespace ssg_test
