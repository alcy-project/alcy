// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "ssg/catalog.h"
#include "ssg/documents.h"
#include "ssg/markdown.h"

namespace ssg {

// The site's pages: the shell every page shares, the article its
// blocks render to, the table of contents beside it, and the
// previous/next pager. Every label is stamped in the page's
// language, so nothing has to be translated at runtime.

struct Stamp {
  // The tag of a release build, the commit's short sha otherwise.
  std::string text;
  std::string url;
};

// A tree of pages the site publishes: the guide and the language
// reference. Both are a contents list beside an article and a pager,
// so one renderer serves both.
struct Tree {
  // The URL segment the tree is rooted at, e.g. `guide`.
  std::string_view segment;
  std::string_view label_key;    // nav and heading label
  std::string_view summary_key;  // meta description and tagline
  // Set when the reading order continues somewhere the page list
  // cannot see (the guide ends where the reference begins).
  std::string_view continues_into_segment;  // "" when nothing
  std::string_view continues_into_label_key;
};

constexpr Tree GUIDE_TREE{.segment = "guide",
                          .label_key = "site.guide",
                          .summary_key = "meta.guide",
                          .continues_into_segment = "reference",
                          .continues_into_label_key = "site.reference"};
constexpr Tree REFERENCE_TREE{.segment = "reference",
                              .label_key = "site.reference",
                              .summary_key = "meta.reference",
                              .continues_into_segment = "",
                              .continues_into_label_key = ""};
constexpr Tree TOOLS_TREE{.segment = "tools",
                          .label_key = "site.tools",
                          .summary_key = "meta.tools",
                          .continues_into_segment = "",
                          .continues_into_label_key = ""};

struct GuideSection {
  std::string id;
  std::string text;
  const Block* block = nullptr;
  usize programs = 0;
};

// The page's sections: its level-2 headings with unique ids.
std::vector<GuideSection> sections_of(const Blocks& blocks);

// The slug a heading anchors at: lowercased, runs collapsed.
std::string heading_slug(std::string_view text);

struct PageOptions {
  const Catalog* catalog = nullptr;
  const std::vector<DocPage>* pages = nullptr;
  const Tree* tree = nullptr;
  std::string_view language;
  // The theme bootstrap, inlined into the head.
  std::string_view boot;
  // The build's provenance, shown in the footer.
  Stamp stamp;
  // Returns the HTML for a code block's source.
  std::function<std::string(std::string_view)> highlight;
  // Resolves and audits the link; the default keeps the URL.
  ResolveLink resolve_link = nullptr;
  void* resolve_context = nullptr;
};

std::string render_guide_page(const DocPage& page, const PageOptions& options);
std::string render_tree_index(const std::vector<DocPage>& pages,
                              const Catalog& catalog,
                              std::string_view language,
                              std::string_view boot,
                              const Stamp& stamp,
                              const Tree& tree);
std::string render_not_found(
    std::string_view boot,
    const Stamp& stamp,
    const std::function<std::string(std::string_view)>& highlight);

// One header for every page, stamped with the same markup.
std::string site_header(const Catalog& catalog,
                        std::string_view route,
                        std::string_view language,
                        std::string_view tree_path,
                        std::string_view current_segment);

std::string footer(const Stamp& stamp);

}  // namespace ssg
