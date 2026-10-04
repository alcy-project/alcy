// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <deque>
#include <optional>
#include <string>
#include <string_view>

#include "fpag/base/limits.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/memory_mapped_file.h"

namespace source {

// Identity of a source file, minted solely by SourceManager. Every other
// module treats it as an opaque key (notably diag::Span, which locates
// views into the files owned here).
using FileId = u32;
constexpr FileId UNKNOWN_FILE = U32_MAX;

enum class SourceError : u8 {
  OpenFailed,
  MapFailed,
};

// Owns loaded source files. Files are memory-mapped; a virtual source is
// held in memory under a name of the caller's choosing. Either way the
// manager owns the bytes, so a caller may hand over a buffer that dies
// immediately.
//
// This is the impure shell: filesystem access lives here so compiler
// passes only ever see views and ids.
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

  // Adds a source that has no file behind it. `name` is what diagnostics
  // and module naming see, and need not be a path: it may be a bare
  // filename, an editor buffer name, or anything else that identifies
  // the text. An id already handed out for the same name is returned
  // unchanged, and `bytes` then has to agree, so a caller that adds a
  // name twice gets the first content rather than two ids for one name.
  FileId add_virtual(std::string_view name, std::string_view bytes);

  // Checked accessors: std::nullopt means `id` does not name a loaded
  // file, which keeps an unknown id distinguishable from a loaded file
  // whose content (or name) happens to be empty.
  std::optional<std::string_view> bytes(FileId id) const;
  std::optional<std::string_view> name(FileId id) const;
  // True when the source is memory-mapped rather than held in memory.
  bool is_mapped(FileId id) const;
  usize file_count() const { return entries_.size(); }

 private:
  struct Entry {
    io::MemoryMappedFile mapping;
    // The name as the caller gave it: a path for a mapped file, an
    // arbitrary label for a virtual one.
    std::string path;
    // Index into `virtuals_`, or NO_VIRTUAL for a mapped file.
    u32 virtual_index = NO_VIRTUAL;
  };

  static constexpr u32 NO_VIRTUAL = U32_MAX;

  // A deque, not a vector: a view into a `std::string` the manager hands
  // out has to survive the next load, and adding an entry would otherwise
  // move every string already there. A short name is stored inside the
  // string object rather than on the heap, so a reallocating container
  // would invalidate the view outright.
  std::deque<Entry> entries_;
  // Likewise: the bytes of a virtual source are handed out as a view.
  std::deque<std::string> virtuals_;
};

}  // namespace source
