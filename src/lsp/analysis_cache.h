// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lsp/analysis.h"
#include "lsp/document.h"
#include "lsp/workspace.h"

namespace lsp {

// One checked reading per package, shared by every open document of that
// package. A request for a buffer whose package did not change reuses the
// previous reading; changing any contributing file rebuilds it once for
// all the package's buffers.
class AnalysisCache {
 public:
  AnalysisCache() = default;
  explicit AnalysisCache(const DocumentStore& documents)
      : documents_(&documents) {}

  const DocumentAnalysis& get(const Document& document);

  // The reading that owns this document's diagnostics. The bag on its
  // context holds the whole package's findings.
  const PackageAnalysis& owner_of(const Document& document) {
    return get(document).owner();
  }

  void set_encoding(PositionEncoding encoding) { encoding_ = encoding; }

  // Without dynamic watcher registration the server gets no notice of an
  // external edit, so a request must re-read the workspace signature.
  void set_watched_files(bool watched) { watched_files_ = watched; }

  // Whether two buffers share a package key, for re-publishing diagnostics
  // to a sibling when one file of the package changes.
  bool same_package(const Document& lhs, const Document& rhs) const {
    return package_key(lhs) == package_key(rhs);
  }

  // A package key and a content signature are stable between the
  // notifications that can change them. Requests reuse both instead of
  // walking the workspace and hashing every source on every hover.
  void invalidate(const Document& document);
  void invalidate_all() {
    package_keys_.clear();
    signatures_.clear();
  }

  void erase(std::string_view uri);

  // Drops package readings no open document's entry still names.

  [[nodiscard]] const Document* open_document(std::string_view uri) const {
    return documents_ == nullptr ? nullptr : documents_->find(uri);
  }

  // Drops every reading, for a notification that a file outside the
  // client's open set changed on disk.
  void clear() {
    entries_.clear();
    packages_.clear();
    invalidate_all();
  }

 private:
  struct Entry {
    std::unique_ptr<DocumentAnalysis> analysis;
    std::string package_key;
    std::string cache_key;
  };

  std::vector<workspace::BufferView> buffers_for(const Document& focus) const;
  std::string compute_package_key(const Document& focus) const;
  const std::string& package_key(const Document& focus) const;
  u64 stamp_for(const Document& focus, const std::string& pkg_key) const;
  // Drops package readings no open document's entry still names.
  void prune_packages();

  const DocumentStore* documents_ = nullptr;
  PositionEncoding encoding_ = PositionEncoding::Utf16;
  bool watched_files_ = false;
  std::unordered_map<std::string, Entry> entries_;
  std::unordered_map<std::string, std::shared_ptr<PackageAnalysis>> packages_;
  struct SignatureEntry {
    u64 stamp = 0;
    std::string value;
  };
  mutable std::unordered_map<std::string, std::string> package_keys_;
  std::unordered_map<std::string, SignatureEntry> signatures_;
};

}  // namespace lsp
