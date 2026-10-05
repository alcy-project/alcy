// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "source/source.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/file_handle.h"
#include "fpag/io/memory_mapped_file.h"

namespace source {

base::Result<FileId, SourceError> SourceManager::load(std::string_view path) {
  const auto known = by_name_.find(path);
  if (known != by_name_.end()) {
    return base::make_ok(known->second);
  }

  // Copy first: FileHandle::open requires a null-terminated path, which
  // only the owned copy guarantees.
  Entry entry;
  entry.path = std::string(path);

  io::FileHandle handle;
  if (!handle.open(entry.path, io::FileAccess::Read)) {
    return base::make_err(SourceError::OpenFailed);
  }

  const usize size = handle.get_size();
  if (size > 0 && !entry.mapping.map(handle, 0, size)) {
    return base::make_err(SourceError::MapFailed);
  }
  entries_.push_back(std::move(entry));
  const FileId id = static_cast<FileId>(entries_.size() - 1);
  by_name_.emplace(entries_.back().path, id);
  return base::make_ok(id);
}

FileId SourceManager::add_virtual(std::string_view name,
                                  std::string_view bytes) {
  // One id per name, matching load(): two ids for one name would let a
  // caller see two different contents under a single label.
  const auto known = by_name_.find(name);
  if (known != by_name_.end()) {
    return known->second;
  }
  virtuals_.emplace_back(bytes);
  Entry entry;
  entry.path = std::string(name);
  entry.virtual_index = static_cast<u32>(virtuals_.size() - 1);
  entries_.push_back(std::move(entry));
  const FileId id = static_cast<FileId>(entries_.size() - 1);
  by_name_.emplace(entries_.back().path, id);
  return id;
}

std::optional<std::string_view> SourceManager::bytes(FileId id) const {
  if (id >= entries_.size()) {
    return std::nullopt;
  }
  const Entry& entry = entries_[id];
  if (entry.virtual_index != NO_VIRTUAL) {
    return std::string_view{virtuals_[entry.virtual_index]};
  }
  if (!entry.mapping.is_mapped()) {
    // A loaded file of zero bytes maps nothing; it is still known.
    return std::string_view{};
  }
  return std::string_view{reinterpret_cast<const char*>(entry.mapping.data()),
                          entry.mapping.size()};
}

std::optional<std::string_view> SourceManager::name(FileId id) const {
  if (id >= entries_.size()) {
    return std::nullopt;
  }
  return entries_[id].path;
}

bool SourceManager::is_mapped(FileId id) const {
  if (id >= entries_.size()) {
    return false;
  }
  return entries_[id].virtual_index == NO_VIRTUAL;
}

}  // namespace source
