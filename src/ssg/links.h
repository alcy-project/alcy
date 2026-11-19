// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ssg/documents.h"

namespace ssg {

// The link audit. Every link a page renders goes through `resolve`:
// a page-to-page `.md` link becomes the built `.html` in the same
// language's tree, a link to anything else in the repository becomes
// a GitHub URL, a link starting with `/` names a path from the
// language tree's root, and a link into the built site must point at
// something that exists. A target that resolves nowhere is recorded
// and fails the build rather than publishing a dead link.

struct LinkContext {
  // The pages are the plan, not the directory's current contents.
  // The audit consults this set to satisfy page-to-page links.
  const std::vector<DocPage>* pages = nullptr;
  // Whether a path relative to the built site's root exists.
  std::function<bool(std::string_view)> site_exists;
  // Whether a path relative to the repository root exists.
  std::function<bool(std::string_view)> repo_exists;
};

class LinkAudit {
 public:
  explicit LinkAudit(LinkContext context);
  std::string resolve(std::string_view url, const DocPage& page);
  const std::vector<std::string>& errors() const { return errors_; }

 private:
  LinkContext context_;
  std::vector<std::string> errors_;
  std::unordered_map<std::string, const DocPage*> pages_by_source_;
};

}  // namespace ssg
