// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/links.h"

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

#include "ssg/documents.h"
#include "ssg/paths.h"

namespace ssg {
namespace {

constexpr std::string_view REPO_BLOB_URL =
    "https://github.com/alcy-project/alcy/blob/main";

// A link scheme or protocol-relative URL is external and left alone.
bool is_external(std::string_view url) {
  if (url.size() >= 2 && url.compare(0, 2, "//") == 0) {
    return true;
  }
  if (url.empty() || !std::isalpha(static_cast<u8>(url.front()))) {
    return false;
  }
  usize index = 1;
  while (index < url.size()) {
    const char c = url[index];
    if (c == ':') {
      return true;
    }
    if (!std::isalnum(static_cast<u8>(c)) && c != '+' && c != '-' && c != '.') {
      return false;
    }
    index++;
  }
  return false;
}

std::string prefix_of_language(std::string_view language) {
  return language == "en" ? std::string() : std::string(language) + "/";
}

std::string tree_of(const DocPage& page) {
  std::string route = page.route;
  const std::string prefix = prefix_of_language(page.language);
  if (!prefix.empty() && route.starts_with(prefix)) {
    route.erase(0, prefix.size());
  }
  const usize slash = route.find('/');
  return slash == std::string::npos ? route : route.substr(0, slash);
}

std::string tree_source_root(std::string_view tree) {
  if (tree == "guide") {
    return "docs/guide";
  }
  if (tree == "reference") {
    return "docs/spec";
  }
  if (tree == "tools") {
    return "docs/tools";
  }
  return std::string();
}

// Guide sources are filed under docs/guide/<lang>/; spec and tools
// sources are not. Drop that first segment for guide sources so a link
// resolves to one tree-relative key regardless of which language's file
// a page uses - including a translation that fell back to the default.
std::string tree_source_rel(std::string_view tree,
                            std::string_view source_rel) {
  if (tree != "guide") {
    return std::string(source_rel);
  }
  const usize slash = source_rel.find('/');
  return slash == std::string_view::npos
             ? std::string(source_rel)
             : std::string(source_rel.substr(slash + 1));
}

struct SplitUrl {
  std::string_view path;
  std::string_view suffix;
};

SplitUrl split_target(std::string_view url) {
  const usize cut = url.find_first_of("?#");
  if (cut == std::string_view::npos) {
    return {url, std::string_view()};
  }
  return {url.substr(0, cut), url.substr(cut)};
}

}  // namespace

LinkAudit::LinkAudit(LinkContext context) : context_(std::move(context)) {
  if (context_.pages == nullptr) {
    return;
  }
  for (const DocPage& page : *context_.pages) {
    const std::string tree = tree_of(page);
    pages_by_source_.emplace(page.language + "\0" + tree + "\0" +
                                 tree_source_rel(tree, page.source_rel),
                             &page);
  }
}

std::string LinkAudit::resolve(std::string_view url, const DocPage& page) {
  const auto fail = [&](std::string_view detail) {
    errors_.push_back(std::string(page.source_rel) + ": [" + std::string(url) +
                      "] " + std::string(detail));
    return std::string(url);
  };
  if (url.empty() || url.front() == '#') {
    return std::string(url);
  }
  if (is_external(url)) {
    return std::string(url);
  }

  const SplitUrl split = split_target(url);
  if (split.path.empty()) {
    return std::string(url);
  }
  const std::string path(split.path);
  const std::string suffix(split.suffix);

  if (path.ends_with(".md")) {
    const std::string tree = tree_of(page);
    const std::string root = tree_source_root(tree);
    if (!root.empty()) {
      const std::string normalized = normalize_posix(
          join_posix({root, dirname_posix(page.source_rel), path}));
      if (normalized == root || normalized.starts_with(root + "/")) {
        const std::string stripped = normalized == root
                                         ? std::string()
                                         : normalized.substr(root.size() + 1);
        const auto target =
            pages_by_source_.find(page.language + "\0" + tree + "\0" +
                                  tree_source_rel(tree, stripped));
        if (target == pages_by_source_.end()) {
          return fail("does not name a " + tree + " page (" + stripped + ")");
        }
        return relative_posix(dirname_posix(page.route),
                              target->second->route) +
               suffix;
      }
      const std::string& repo_path = normalized;
      if (!context_.repo_exists || !context_.repo_exists(repo_path)) {
        return fail("does not exist in the repository (" + repo_path + ")");
      }
      return std::string(REPO_BLOB_URL) + "/" + repo_path + suffix;
    }
    const std::string repo_path =
        normalize_posix(join_posix(dirname_posix(page.source_rel), path));
    if (!context_.repo_exists || !context_.repo_exists(repo_path)) {
      return fail("does not exist in the repository (" + repo_path + ")");
    }
    return std::string(REPO_BLOB_URL) + "/" + repo_path + suffix;
  }

  if (path.starts_with('/')) {
    const std::string candidate =
        prefix_of_language(page.language) + path.substr(1);
    if (!context_.site_exists || !context_.site_exists(candidate)) {
      if (candidate.empty()) {
        return fail("does not exist in the built site (index.html)");
      }
      return fail("does not exist in the built site (" + candidate + ")");
    }
    std::string rebased = relative_posix(dirname_posix(page.route), candidate);
    const bool trailing = candidate.empty() || candidate.ends_with('/');
    if (trailing && (rebased.empty() || rebased.back() != '/')) {
      rebased += '/';
    }
    return rebased + suffix;
  }

  const std::string normalized =
      normalize_posix(join_posix(dirname_posix(page.route), path));
  const std::string site_path = normalized == "." ? std::string() : normalized;
  if (context_.site_exists && !context_.site_exists(site_path)) {
    if (site_path.empty()) {
      return fail("does not exist in the built site (index.html)");
    }
    return fail("does not exist in the built site (" + site_path + ")");
  }
  return std::string(url);
}

}  // namespace ssg
