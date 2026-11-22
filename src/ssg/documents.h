// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "fpag/base/result.h"
#include "ssg/catalog.h"
#include "ssg/markdown.h"

namespace ssg {

// What a source document is, and what one language's page for it is.
// The guide and the reference publish different sets of files found in
// different ways - a filename convention against an explicit reading
// order - but everything after the discovery is the same: a source
// becomes one page per language, and only the chrome differs.

struct DocSource {
  // The path under the documentation directory, e.g. `types.md`.
  std::string source_rel;
  // The language this source is written in. Every guide language lives
  // under its own docs/guide/<lang>/ directory; spec and tools sources
  // carry no language.
  std::string language;
  // The path inside one language tree, e.g. `reference/types.html`.
  std::string path;
  usize order = 0;
  std::string title;
  Blocks blocks;
  std::vector<std::string> parsed_path_parts;
};

struct DocPage {
  std::string source_rel;
  std::string path;
  usize order = 0;
  std::string title;
  Blocks blocks;
  std::string language;
  // The path under the built site's root, e.g. `ja/reference/types.html`.
  std::string route;
};

// The regular files directly under `dir`, as names, sorted. The
// immediate subdirectory names go to `subdirs` when it is not null,
// also sorted, so a caller can walk a tree without a second API.
base::Result<std::vector<std::string>, std::string> list_directory(
    std::string_view dir,
    std::vector<std::string>* subdirs);

// Every `.md` file directly under `dir`, as file names, sorted.
base::Result<std::vector<std::string>, std::string> markdown_files(
    std::string_view dir);

// A title is the first line: every page opens with the heading that
// names it, so a page's title is its source's heading rather than a
// second place to keep it in step.
base::Result<std::string, std::string> title_of(const Blocks& blocks,
                                                std::string_view source_rel);

std::vector<DocPage> pages_for_language(const Catalog& catalog,
                                        const std::vector<DocSource>& sources,
                                        std::string_view language);

}  // namespace ssg
