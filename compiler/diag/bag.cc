// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/bag.h"

#include <cstring>
#include <initializer_list>
#include <optional>
#include <string_view>

#include "debug/dcheck.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace diag {

u32 DiagBag::push(Severity severity,
                  std::optional<Code> code,
                  Span primary,
                  bool has_primary,
                  std::string_view message) {
  if (size_ == capacity_) {
    // Saturate rather than wrap: a u32 overflow would shrink the
    // capacity and the next push would write out of bounds.
    const u32 grown = capacity_ == 0        ? INITIAL_CAPACITY
                      : capacity_ > ~0u / 2 ? ~0u
                                            : capacity_ * 2;
    DCHECK(grown > capacity_);
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

void DiagBag::merge(const DiagBag& other) {
  for (u32 i = 0; i < other.size_; ++i) {
    const Diagnostic& from = other.entries_[i];
    // The code crosses as-is, including its absence: a message from
    // outside a check area has none, and a default-constructed code
    // would render as a check nobody allocated.
    const u32 at = push(from.severity, from.code, from.primary_span,
                        from.has_primary_span, from.message);
    if (from.label_count == 0) {
      continue;
    }
    Label* const labels = static_cast<Label*>(
        arena_->alloc(sizeof(Label) * from.label_count, alignof(Label)));
    for (u32 l = 0; l < from.label_count; ++l) {
      labels[l] = from.labels[l];
      // The message is a view into the other bag's arena, which this bag
      // does not own and cannot read once the caller is done with it.
      labels[l].message = intern(from.labels[l].message);
    }
    entries_[at].labels = labels;
    entries_[at].label_count = from.label_count;
  }
}

}  // namespace diag
