// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/memory_mapped_file.h"

namespace source {

// Identity of a source file, minted solely by SourceManager. Every other
// module treats it as an opaque key (notably diag::Span, which locates
// views into the files owned here).
using FileId = u32;
constexpr FileId kUnknownFile = std::numeric_limits<FileId>::max();

enum class SourceError : u8 {
  OpenFailed,
  MapFailed,
};

// Owns loaded source files (memory-mapped; empty files map to empty views).
// This is the impure shell: filesystem access lives here so compiler passes
// only ever see views and ids.
class SourceManager {
 public:
  SourceManager() = default;
  ~SourceManager() = default;

  SourceManager(const SourceManager&) = delete;
  SourceManager& operator=(const SourceManager&) = delete;
  SourceManager(SourceManager&&) noexcept = default;
  SourceManager& operator=(SourceManager&&) noexcept = default;

  // Loads a file, or returns the existing id when already loaded.
  base::Result<FileId, SourceError> load(std::string_view path);

  std::string_view bytes(FileId id) const;
  std::string_view name(FileId id) const;
  usize file_count() const { return entries_.size(); }

 private:
  struct Entry {
    io::MemoryMappedFile mapping;
    std::string path;
  };

  std::vector<Entry> entries_;
};

}  // namespace source
