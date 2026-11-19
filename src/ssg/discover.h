// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "fpag/base/result.h"
#include "ssg/documents.h"
#include "ssg/markdown.h"

namespace ssg {

// The discovery passes: which sources the site publishes, in what
// order, and what each one's path is. The guide orders by filename,
// the reference and the tools tree by an explicit list with the rest
// appended in name order.

base::Result<std::vector<DocSource>, std::string> discover_guide(
    const std::filesystem::path& docs_dir);

base::Result<std::vector<DocSource>, std::string> discover_spec(
    const std::filesystem::path& spec_dir);

struct SpecAsset {
  std::string source_rel;
  std::string path;
};

// Files the reference publishes next to its pages: the normative
// grammar the prose points at.
base::Result<std::vector<SpecAsset>, std::string> discover_spec_assets(
    const std::filesystem::path& spec_dir);

// The prose of the reference and the tools tree names sibling pages as
// backticked file.md; the ones with a page become links, the rest stay
// text. Exposed for the tests that hold the rewrite in place.
Blocks link_sibling_references(Blocks blocks,
                               const std::set<std::string>& published);

base::Result<std::vector<DocSource>, std::string> discover_tools(
    const std::filesystem::path& tools_dir);

}  // namespace ssg
