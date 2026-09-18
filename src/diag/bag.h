// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <initializer_list>
#include <iterator>
#include <string_view>
#include <utility>

#include "debug/dcheck.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fmt/core.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"

namespace diag {

// Marker error type for fallible phase results: when a phase bails, details
// are already in the bag, so the error itself carries nothing.
struct Fatal {};

// Conventional phase result: T on success, Fatal (details in DiagBag) on
// unrecoverable failure. Recoverable diagnostics never fail the result;
// the driver decides the exit code from DiagBag::has_errors().
template <typename T>
using Fallible = base::Result<T, Fatal>;

// Arena-backed bag of diagnostics gathered during one compilation phase.
//
// The arena is injected, not owned: construct DiagBag over a caller-reserved
// mem::Arena (cold path only - message composition is the only allocation,
// and it bumps the injected arena rather than the heap).
//
// Diagnostics are addressed by stable u32 indices: the entries array can
// relocate as it grows, so the bag never hands out references.
class DiagBag {
 public:
  explicit DiagBag(mem::Arena& arena) : arena_(&arena) {}

  DiagBag(const DiagBag&) = delete;
  DiagBag& operator=(const DiagBag&) = delete;

  template <typename... Args>
  u32 emit(Severity severity,
           u32 code,
           fmt::format_string<Args...> format,
           Args&&... args) {
    return emit_impl(severity, code, Span{}, false, format,
                     std::forward<Args>(args)...);
  }

  template <typename... Args>
  u32 emit(Severity severity,
           u32 code,
           Span primary,
           fmt::format_string<Args...> format,
           Args&&... args) {
    return emit_impl(severity, code, primary, true, format,
                     std::forward<Args>(args)...);
  }

  // Attaches secondary labels to a previously emitted diagnostic. The labels
  // array is copied into the arena.
  void label(u32 index, std::initializer_list<Label> labels);

  Diagnostic& at(u32 index) {
    DCHECK_LT(index, size_);
    return entries_[index];
  }
  const Diagnostic& at(u32 index) const {
    DCHECK_LT(index, size_);
    return entries_[index];
  }

  bool has_errors() const { return error_count_ > 0; }
  u32 error_count() const { return error_count_; }
  u32 warning_count() const { return warning_count_; }
  u32 size() const { return size_; }

  // Iteration for renderers. Diagnostics are stored newest-last.
  template <typename F>
  void for_each(F&& f) const {
    for (u32 i = 0; i < size_; ++i) {
      f(entries_[i]);
    }
  }

 private:
  template <typename... Args>
  u32 emit_impl(Severity severity,
                u32 code,
                Span primary,
                bool has_primary,
                fmt::format_string<Args...> format,
                Args&&... args) {
    fmt::memory_buffer out;
    fmt::format_to(std::back_inserter(out), format,
                   std::forward<Args>(args)...);
    return push(severity, code, primary, has_primary, {out.data(), out.size()});
  }

  // Appends a diagnostic with an already-composed message. Copies the
  // message into the arena; grows the entries array as needed. Returns the
  // stable index of the new entry.
  u32 push(Severity severity,
           u32 code,
           Span primary,
           bool has_primary,
           std::string_view message);

  // Copies bytes into the arena and returns a view of the copy.
  std::string_view intern(std::string_view bytes) const;

  mem::Arena* arena_;
  // Arena-owned array of all diagnostics, in emission order.
  Diagnostic* entries_ = nullptr;
  u32 capacity_ = 0;
  u32 size_ = 0;
  u32 error_count_ = 0;
  u32 warning_count_ = 0;

  static constexpr u32 kInitialCapacity = 8;
};

}  // namespace diag
