// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/bag.h"

#include <cstring>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

#include "debug/dcheck.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace diag {

u32 DiagBag::grown_capacity() const {
  // Saturate rather than wrap: a u32 overflow would shrink the capacity
  // and the next push would write out of bounds.
  return capacity_ == 0        ? INITIAL_CAPACITY
         : capacity_ > ~0u / 2 ? ~0u
                               : capacity_ * 2;
}

bool DiagBag::has_room_for(usize bytes) const {
  return arena_->capacity() - arena_->size() >= bytes;
}

u32 DiagBag::push(Severity severity,
                  std::optional<Code> code,
                  Span primary,
                  bool has_primary,
                  std::string_view message) {
  // A diagnostic is stored whole or not at all: a clipped message and the
  // entry pointing at it must be decided together, or the entry reads
  // bytes that were never copied.
  char clipped[MAX_MESSAGE_BYTES + 4];
  if (message.size() > MAX_MESSAGE_BYTES) {
    std::memcpy(clipped, message.data(), MAX_MESSAGE_BYTES);
    std::memcpy(clipped + MAX_MESSAGE_BYTES, "...", 3);
    message = std::string_view(clipped, MAX_MESSAGE_BYTES + 3);
  }
  const usize growth =
      size_ == capacity_ ? sizeof(Diagnostic) * grown_capacity() : 0;
  if (!has_room_for(growth + message.size())) {
    // The arena is spent. The diagnostic is counted but not stored, and
    // the caller is told by `dropped_count` at the end of the run.
    ++dropped_count_;
    if (severity == Severity::Error) {
      ++dropped_errors_;
    }
    return size_;
  }
  if (growth != 0) {
    DCHECK(grown_capacity() > capacity_);
    Diagnostic* const mem = static_cast<Diagnostic*>(arena_->alloc(
        sizeof(Diagnostic) * grown_capacity(), alignof(Diagnostic)));
    for (u32 i = 0; i < size_; ++i) {
      mem[i] = entries_[i];
    }
    entries_ = mem;
    capacity_ = grown_capacity();
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
  // The caller has checked that this fits, so the arena answers with
  // room; the null test is what an accounting mistake degrades to rather
  // than a write through nothing.
  char* const mem =
      static_cast<char*>(arena_->alloc(bytes.size(), alignof(char)));
  if (mem == nullptr) {
    return {};
  }
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
  usize needed = sizeof(Label) * labels.size();
  for (const Label& label : labels) {
    needed += label.message.size();
  }
  if (!has_room_for(needed)) {
    // A label points at a second span; unlike the diagnostic it belongs
    // to, dropping it leaves a whole rendering rather than a hole.
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

namespace {

// Two diagnostics are one problem seen twice when everything a renderer
// reads matches: severity, code, span, message, and labels.
bool same_span(Span a, Span b) {
  return a.file == b.file && a.offset == b.offset && a.length == b.length;
}

bool same_diagnostic(const Diagnostic& a, const Diagnostic& b) {
  if (a.severity != b.severity || a.code != b.code ||
      a.has_primary_span != b.has_primary_span || a.message != b.message ||
      a.label_count != b.label_count) {
    return false;
  }
  if (a.has_primary_span && !same_span(a.primary_span, b.primary_span)) {
    return false;
  }
  for (u32 i = 0; i < a.label_count; ++i) {
    if (!same_span(a.labels[i].span, b.labels[i].span) ||
        a.labels[i].message != b.labels[i].message) {
      return false;
    }
  }
  return true;
}

// FNV-1a over the fields `same_diagnostic` reads, so entries that hash
// apart are known to differ and the ones that hash together are settled
// by comparing them whole.
u64 hash_diagnostic(const Diagnostic& d) {
  u64 hash = 14695981039346656037ull;
  const auto mix = [&hash](u64 value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  const auto mix_text = [&mix](std::string_view text) {
    for (const char c : text) {
      mix(static_cast<u64>(static_cast<unsigned char>(c)));
    }
  };
  mix(static_cast<u64>(d.severity));
  mix(d.code.has_value() ? (static_cast<u64>(d.code->stage) << 8) | d.code->id
                         : 1ull << 32);
  mix(d.has_primary_span ? 1 : 0);
  if (d.has_primary_span) {
    mix(d.primary_span.file);
    mix(d.primary_span.offset);
    mix(d.primary_span.length);
  }
  mix_text(d.message);
  mix(d.label_count);
  for (u32 i = 0; i < d.label_count; ++i) {
    mix(d.labels[i].span.file);
    mix(d.labels[i].span.offset);
    mix(d.labels[i].span.length);
    mix_text(d.labels[i].message);
  }
  return hash;
}

}  // namespace

void DiagBag::dedup() {
  if (size_ < 2) {
    return;
  }
  // Open addressing, half full at most, so a duplicate lands on the same
  // probe chain as its first occurrence.
  usize table_size = 8;
  while (table_size < static_cast<usize>(size_) * 2) {
    table_size *= 2;
  }
  constexpr u32 EMPTY = ~0u;
  std::vector<u32> table(table_size, EMPTY);
  u32 kept = 0;
  u32 errors = 0;
  u32 warnings = 0;
  for (u32 i = 0; i < size_; ++i) {
    const u64 hash = hash_diagnostic(entries_[i]);
    usize slot = static_cast<usize>(hash) & (table_size - 1);
    bool duplicate = false;
    while (table[slot] != EMPTY) {
      if (same_diagnostic(entries_[table[slot]], entries_[i])) {
        duplicate = true;
        break;
      }
      slot = (slot + 1) & (table_size - 1);
    }
    if (duplicate) {
      continue;
    }
    table[slot] = kept;
    if (i != kept) {
      entries_[kept] = entries_[i];
    }
    if (entries_[kept].severity == Severity::Error) {
      ++errors;
    } else if (entries_[kept].severity == Severity::Warning) {
      ++warnings;
    }
    ++kept;
  }
  size_ = kept;
  error_count_ = errors;
  warning_count_ = warnings;
}

void DiagBag::merge(const DiagBag& other) {
  for (u32 i = 0; i < other.size_; ++i) {
    const Diagnostic& from = other.entries_[i];
    // The code crosses as-is, including its absence: a message from
    // outside a check area has none, and a default-constructed code
    // would render as a check nobody allocated.
    const u32 at = push(from.severity, from.code, from.primary_span,
                        from.has_primary_span, from.message);
    if (at >= size_ || from.label_count == 0) {
      // Out of range means this bag was full and the diagnostic was
      // dropped; its labels have nothing to attach to.
      continue;
    }
    usize needed = sizeof(Label) * from.label_count;
    for (u32 l = 0; l < from.label_count; ++l) {
      needed += from.labels[l].message.size();
    }
    if (!has_room_for(needed)) {
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
  // What the other bag could not hold crosses too: the count is what a
  // caller reports, and an error among them still fails this bag.
  dropped_count_ += other.dropped_count_;
  dropped_errors_ += other.dropped_errors_;
}

}  // namespace diag
