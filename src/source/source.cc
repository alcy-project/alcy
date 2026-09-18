// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "source/source.h"

#include <string>
#include <string_view>
#include <utility>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/file_handle.h"
#include "fpag/io/memory_mapped_file.h"

namespace source {

base::Result<FileId, SourceError> SourceManager::load(std::string_view path) {
  for (FileId i = 0; i < static_cast<FileId>(entries_.size()); ++i) {
    if (entries_[i].path == path) {
      return base::make_ok(i);
    }
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
  return base::make_ok(static_cast<FileId>(entries_.size() - 1));
}

std::string_view SourceManager::bytes(FileId id) const {
  if (id >= entries_.size()) {
    return {};
  }
  const Entry& entry = entries_[id];
  if (!entry.mapping.is_mapped()) {
    return {};
  }
  return {reinterpret_cast<const char*>(entry.mapping.data()),
          entry.mapping.size()};
}

std::string_view SourceManager::name(FileId id) const {
  if (id >= entries_.size()) {
    return {};
  }
  return entries_[id].path;
}

}  // namespace source
