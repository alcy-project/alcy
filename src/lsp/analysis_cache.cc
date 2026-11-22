// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/analysis_cache.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/hash.h"
#include "i18n/language.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "lsp/workspace.h"
#include "path/path.h"
#include "source/source.h"

namespace lsp {

namespace {

// The golden-ratio split constant for mixing version numbers into a
// cache stamp; no domain meaning, just a stable decorrelation.
constexpr u64 VERSION_STAMP_MIX = 0x9E3779B97F4A7C15ull;

std::string resolve_uri(std::string_view uri) {
  // Only a file URI names a path the package walk can resolve. An
  // untitled buffer is standalone here just as build_analysis() makes
  // it standalone; treating it as a relative path would key the cache
  // to a package whose fingerprint never includes the buffer's text.
  return workspace::uri_to_path(uri).value_or(std::string{});
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

std::string AnalysisCache::compute_package_key(const Document& focus) const {
  const std::string path = resolve_uri(focus.uri());
  if (path.empty()) {
    return "standalone:" + focus.uri();
  }
  const std::optional<path::Path> root = workspace::find_package_root(path);
  return root.has_value() ? std::string(root->as_view())
                          : "standalone:" + focus.uri();
}

const std::string& AnalysisCache::package_key(const Document& focus) const {
  const auto found = package_keys_.find(focus.uri());
  if (found != package_keys_.end()) {
    return found->second;
  }
  return package_keys_.emplace(focus.uri(), compute_package_key(focus))
      .first->second;
}

u64 AnalysisCache::stamp_for(const Document& focus,
                             const std::string& pkg_key) const {
  // Only the documents that feed this package can invalidate its
  // signature. An edit in an unrelated package must not force a disk
  // re-fingerprint here, and a u64 avoids a string build per request.
  u64 stamp = base::hash64(focus.uri());
  bool focus_in_store = false;
  if (documents_ != nullptr) {
    for (const auto& [uri, document] : documents_->documents()) {
      if (package_key(document) != pkg_key) {
        continue;
      }
      stamp ^= base::hash64(uri) +
               static_cast<u64>(document.version()) * VERSION_STAMP_MIX;
      if (uri == focus.uri()) {
        focus_in_store = true;
      }
    }
  }
  // A direct get() on a document the store does not hold still has to
  // notice its version; otherwise the terms above would cancel it out.
  if (!focus_in_store) {
    stamp ^= static_cast<u64>(focus.version()) * VERSION_STAMP_MIX;
  }
  return stamp;
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
  const u64 stamp = stamp_for(document, pkg_key);
  const auto cached = signatures_.find(pkg_key);
  if (!watched_files_ || cached == signatures_.end() ||
      cached->second.stamp != stamp) {
    const std::vector<workspace::BufferView> buffers = buffers_for(document);
    const std::string value =
        pkg_key.starts_with("standalone:")
            ? pkg_key + ":" + std::to_string(document.version()) + ":" +
                  base::hash128(document.text()).hex()
            : workspace::fingerprint(pkg_key, buffers);
    signatures_[pkg_key] = SignatureEntry{stamp, value};
  }
  const std::string& signature = signatures_.at(pkg_key).value;
  const std::string cache_key = pkg_key + "\0" + signature;

  bool removed = false;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second.package_key == pkg_key &&
        it->second.cache_key != cache_key) {
      it = entries_.erase(it);
      removed = true;
    } else {
      ++it;
    }
  }
  if (removed) {
    prune_packages();
  }

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
