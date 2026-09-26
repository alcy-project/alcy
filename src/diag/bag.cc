// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/bag.h"

#include <cstring>
#include <initializer_list>
#include <string_view>

#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"

namespace diag {

u32 DiagBag::push(Severity severity,
                  u32 code,
                  Span primary,
                  bool has_primary,
                  std::string_view message) {
  if (size_ == capacity_) {
    const u32 grown = capacity_ == 0 ? INITIAL_CAPACITY : capacity_ * 2;
    Diagnostic* const mem = static_cast<Diagnostic*>(
        arena_->alloc(sizeof(Diagnostic) * grown, alignof(Diagnostic)));
    for (u32 i = 0; i < size_; ++i) {
      mem[i] = entries_[i];
    }
    entries_ = mem;
    capacity_ = grown;
  }
  const u32 index = size_++;
  Diagnostic& slot = entries_[index];
  slot.severity = severity;
  slot.code = code;
  slot.message = intern(message);
  slot.has_primary_span = has_primary;
  slot.primary_span = primary;
  slot.labels = nullptr;
  slot.label_count = 0;
  if (severity == Severity::Error) {
    ++error_count_;
  } else if (severity == Severity::Warning) {
    ++warning_count_;
  }
  return index;
}

std::string_view DiagBag::intern(std::string_view bytes) const {
  if (bytes.empty()) {
    return {};
  }
  char* const mem =
      static_cast<char*>(arena_->alloc(bytes.size(), alignof(char)));
  // The arena is caller-reserved; exhaustion is a configuration bug, and the
  // arena already DCHECKs on it. A null here would only follow that.
  std::memcpy(mem, bytes.data(), bytes.size());
  return {mem, bytes.size()};
}

base::Result<void, BagError> DiagBag::label(
    u32 index,
    std::initializer_list<Label> labels) {
  if (index >= size_) {
    return base::make_err(BagError::InvalidIndex);
  }
  if (labels.size() == 0) {
    return base::make_ok();
  }
  Label* const mem = static_cast<Label*>(
      arena_->alloc(sizeof(Label) * labels.size(), alignof(Label)));
  Label* dst = mem;
  for (const Label& label : labels) {
    *dst++ = label;
  }
  entries_[index].labels = mem;
  entries_[index].label_count = static_cast<u32>(labels.size());
  return base::make_ok();
}

}  // namespace diag
