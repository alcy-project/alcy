// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <initializer_list>
#include <iterator>
#include <string_view>
#include <utility>

#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fmt/core.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/messages.h"

namespace diag {

// Marker error type for APIs that report failure through a DiagBag: the
// Result conveys only success or failure because the details are already
// recorded in the bag. Zero-sized, so it costs nothing to return.
struct Reported {};

// Structured failure for DiagBag itself: a diagnostic index that does not
// name an emitted entry.
enum class BagError : u8 {
  InvalidIndex,
};

// Diagnostic-side-channel convention: a phase that accumulates diagnostics
// returns base::Result<T, Reported>. Recoverable diagnostics never fail
// the result; the cli decides the exit code from DiagBag::has_errors().
// Failures with programmatically actionable details use a module-local
// error type instead.

// Arena-backed bag of diagnostics gathered during one compilation phase.
//
// The arena is injected, not owned: construct DiagBag over a caller-reserved
// mem::Arena (cold path only - message composition is the only allocation,
// and it bumps the injected arena rather than the heap).
//
// The language is part of the bag because a diagnostic records the text
// the user was told, not a recipe for telling them: the message is
// composed from the catalog here, and rendering it again renders the
// same text. One invocation reports in one language.
//
// Diagnostics are addressed by stable u32 indices: the entries array can
// relocate as it grows, so the bag never hands out references.
class DiagBag {
 public:
  DiagBag(mem::Arena& arena, i18n::Language language)
      : arena_(&arena), language_(language) {}

  DiagBag(const DiagBag&) = delete;
  DiagBag& operator=(const DiagBag&) = delete;

  // Composes a message from the catalog and appends the diagnostic.
  // Every catalog's format string for K is checked against Args... at
  // compile time, so a translation that does not fit the call site is a
  // build error rather than a diagnostic printed wrong.
  template <i18n::Key K, typename... Args>
  u32 emit(Severity severity, u32 code, Args&&... args) {
    fmt::memory_buffer out;
    i18n::format_to<K>(out, language_, std::forward<Args>(args)...);
    return push(severity, code, {}, false, {out.data(), out.size()});
  }

  template <i18n::Key K, typename... Args>
  u32 emit(Severity severity, u32 code, Span primary, Args&&... args) {
    fmt::memory_buffer out;
    i18n::format_to<K>(out, language_, std::forward<Args>(args)...);
    return push(severity, code, primary, true, {out.data(), out.size()});
  }

  // Appends a diagnostic whose message is composed here rather than from
  // the catalog. Nothing new emits through this: a message the user reads
  // is a catalog entry, and a test's own wording is the one case left.
  template <typename... Args>
  u32 emit_untranslated(Severity severity,
                        u32 code,
                        fmt::format_string<Args...> format,
                        Args&&... args) {
    fmt::memory_buffer out;
    fmt::format_to(std::back_inserter(out), format,
                   std::forward<Args>(args)...);
    return push(severity, code, {}, false, {out.data(), out.size()});
  }

  template <typename... Args>
  u32 emit_untranslated(Severity severity,
                        u32 code,
                        Span primary,
                        fmt::format_string<Args...> format,
                        Args&&... args) {
    fmt::memory_buffer out;
    fmt::format_to(std::back_inserter(out), format,
                   std::forward<Args>(args)...);
    return push(severity, code, primary, true, {out.data(), out.size()});
  }

  // The untranslated form, still reached under its old name while the
  // remaining call sites move to the catalog. It is not overloaded with
  // the keyed form above: a key cannot be deduced from an argument, so
  // the two never both answer a call.
  template <typename... Args>
  u32 emit(Severity severity,
           u32 code,
           fmt::format_string<Args...> format,
           Args&&... args) {
    return emit_untranslated(severity, code, format,
                             std::forward<Args>(args)...);
  }

  template <typename... Args>
  u32 emit(Severity severity,
           u32 code,
           Span primary,
           fmt::format_string<Args...> format,
           Args&&... args) {
    return emit_untranslated(severity, code, primary, format,
                             std::forward<Args>(args)...);
  }

  // Attaches secondary labels to a previously emitted diagnostic. The labels
  // array is copied into the arena. Fails when `index` does not name an
  // emitted diagnostic.
  base::Result<void, BagError> label(u32 index,
                                     std::initializer_list<Label> labels);

  // Checked lookup: returns nullptr when `index` does not name an emitted
  // diagnostic.
  const Diagnostic* at(u32 index) const {
    if (index >= size_) {
      return nullptr;
    }
    return &entries_[index];
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
  i18n::Language language_;
  // Arena-owned array of all diagnostics, in emission order.
  Diagnostic* entries_ = nullptr;
  u32 capacity_ = 0;
  u32 size_ = 0;
  u32 error_count_ = 0;
  u32 warning_count_ = 0;

  static constexpr u32 INITIAL_CAPACITY = 8;
};

}  // namespace diag
