// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/analysis_cache.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "i18n/language.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "lsp/workspace.h"
#include "path/path.h"
#include "source/source.h"

namespace lsp {

namespace {

std::string resolve_uri(std::string_view uri,
                        const std::optional<path::Path>& workspace_root) {
  if (const std::optional<std::string> path = workspace::uri_to_path(uri);
      path.has_value()) {
    return *path;
  }
  if (workspace_root.has_value() && !uri.starts_with('/')) {
    return std::string(workspace_root->join(uri).as_view());
  }
  return {};
}

}  // namespace

std::vector<workspace::BufferView> AnalysisCache::buffers_for(
    const Document& focus) const {
  std::vector<workspace::BufferView> buffers;
  if (documents_ != nullptr) {
    buffers.reserve(documents_->documents().size());
    for (const auto& [uri, document] : documents_->documents()) {
      buffers.push_back(
          workspace::BufferView{uri, document.text(), document.version()});
    }
  } else {
    buffers.push_back(
        workspace::BufferView{focus.uri(), focus.text(), focus.version()});
  }
  return buffers;
}

std::string AnalysisCache::package_key_for(const Document& focus) const {
  const std::string path = resolve_uri(focus.uri(), workspace_root_);
  if (path.empty()) {
    return "standalone:" + focus.uri();
  }
  const std::optional<path::Path> root = workspace::find_package_root(path);
  return root.has_value() ? std::string(root->as_view())
                          : "standalone:" + focus.uri();
}

const std::string& AnalysisCache::package_key(const Document& focus) {
  const auto found = package_keys_.find(focus.uri());
  if (found != package_keys_.end()) {
    return found->second;
  }
  return package_keys_.emplace(focus.uri(), package_key_for(focus))
      .first->second;
}

std::string AnalysisCache::stamp_for(const Document& focus) const {
  std::string out = std::to_string(focus.version());
  if (documents_ != nullptr) {
    for (const auto& [uri, document] : documents_->documents()) {
      out.push_back(';');
      out.append(uri);
      out.push_back(':');
      out.append(std::to_string(document.version()));
    }
  }
  return out;
}

void AnalysisCache::invalidate(const Document& document) {
  const auto key = package_keys_.find(document.uri());
  if (key != package_keys_.end()) {
    signatures_.erase(key->second);
  }
}

const DocumentAnalysis& AnalysisCache::get(const Document& document) {
  const std::string& uri = document.uri();
  const std::string& pkg_key = package_key(document);
  const std::string stamp = stamp_for(document);
  const auto cached = signatures_.find(pkg_key);
  if (cached == signatures_.end() || cached->second.stamp != stamp) {
    const std::vector<workspace::BufferView> buffers = buffers_for(document);
    const std::string value =
        pkg_key.starts_with("standalone:")
            ? pkg_key + ":" + std::to_string(document.version())
            : workspace::fingerprint(pkg_key, buffers);
    signatures_[pkg_key] = SignatureEntry{stamp, value};
  }
  const std::string& signature = signatures_.at(pkg_key).value;
  const std::string cache_key = pkg_key + "\0" + signature;

  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second.package_key == pkg_key &&
        it->second.cache_key != cache_key) {
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  prune_packages();

  const auto found = entries_.find(uri);
  if (found != entries_.end() && found->second.cache_key == cache_key) {
    return *found->second.analysis;
  }

  auto inserted = packages_.find(cache_key);
  if (inserted == packages_.end()) {
    const std::vector<workspace::BufferView> buffers = buffers_for(document);
    std::shared_ptr<PackageAnalysis> owner = build_analysis(
        buffers, document.uri(), i18n::Language::EnUs, encoding_);
    owner->fingerprint = signature;
    inserted = packages_.emplace(cache_key, std::move(owner)).first;
  }
  const PackageAnalysis& owner = *inserted->second;

  const auto file = owner.files_by_uri.find(uri);
  source::FileId root = source::UNKNOWN_FILE;
  usize package_index = owner.checked.size();
  if (file != owner.files_by_uri.end()) {
    root = file->second;
    const auto package = owner.package_of.find(root);
    if (package != owner.package_of.end()) {
      package_index = package->second;
    }
  }
  auto analysis =
      std::make_unique<DocumentAnalysis>(inserted->second, root, package_index);
  const DocumentAnalysis& result = *analysis;
  entries_.insert_or_assign(uri,
                            Entry{std::move(analysis), pkg_key, cache_key});
  return result;
}

void AnalysisCache::erase(std::string_view uri) {
  entries_.erase(std::string(uri));
  prune_packages();
}

void AnalysisCache::prune_packages() {
  for (auto it = packages_.begin(); it != packages_.end();) {
    bool used = false;
    for (const auto& entry : entries_) {
      if (entry.second.cache_key == it->first) {
        used = true;
        break;
      }
    }
    if (!used) {
      it = packages_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace lsp
