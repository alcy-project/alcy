// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/documents.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ssg/catalog.h"
#include "ssg/markdown.h"

namespace ssg {

base::Result<std::vector<std::string>, std::string> markdown_files(
    const std::filesystem::path& dir) {
  std::vector<std::string> names;
  std::error_code error;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(dir, error)) {
    if (error) {
      return base::make_err("cannot list the directory: " + dir.string() +
                            ": " + error.message());
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::filesystem::path name = entry.path().filename();
    if (name.string().ends_with(".md")) {
      names.push_back(name.string());
    }
  }
  if (error) {
    return base::make_err("cannot list the directory: " + dir.string() + ": " +
                          error.message());
  }
  std::sort(names.begin(), names.end());
  return base::make_ok(std::move(names));
}

base::Result<std::string, std::string> title_of(const Blocks& blocks,
                                                std::string_view source_rel) {
  if (blocks.empty() || blocks.front().type != BlockType::Heading ||
      blocks.front().level != 1) {
    return base::make_err(std::string(source_rel) +
                          ": the file must open with a level-1 heading");
  }
  return base::make_ok(blocks.front().text);
}

std::vector<DocPage> pages_for_language(const Catalog& catalog,
                                        const std::vector<DocSource>& sources,
                                        std::string_view language) {
  std::vector<DocPage> pages;
  pages.reserve(sources.size());
  const std::string prefix(prefix_of(catalog, language));
  for (const DocSource& source : sources) {
    DocPage page;
    page.source_rel = source.source_rel;
    page.path = source.path;
    page.order = source.order;
    page.title = source.title;
    page.blocks = source.blocks;
    page.language = std::string(language);
    page.route = prefix + source.path;
    pages.push_back(std::move(page));
  }
  return pages;
}

}  // namespace ssg
