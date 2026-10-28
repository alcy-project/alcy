// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <initializer_list>
#include <iterator>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "diag/diagnostic.h"
#include "diag/message_meta.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fmt/core.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "i18n/messages.h"

namespace diag {

// The id half of a code, as whatever enumerator the reporting component
// wrote. A scoped enum does not convert to its underlying type, so the
// parameter is the type itself and the narrowing happens once inside
// `emit`; the alternative is a cast at every call site. The constraint is
// what keeps a bare number out, which is the reason the enums exist.
template <typename Id>
concept DiagnosticId = std::is_enum_v<Id>;

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

  // Composes a message from the catalog and appends the diagnostic. The
  // severity and the phase are the message's own facts - the catalog
  // carries them - so a call site names the message, the code tooling
  // matches on, and the arguments. Every catalog's format string for K is
  // checked against Args... at compile time, so a translation that does
  // not fit the call site is a build error rather than a diagnostic
  // printed wrong.
  template <i18n::Key K, DiagnosticId Id, typename... Args>
  u32 emit(Id id, Args&&... args) {
    const MessageMeta meta = MESSAGE_META[static_cast<u16>(K)];
    return emit_with<K>(meta.severity, meta.stage, static_cast<u8>(id), {},
                        false, std::forward<Args>(args)...);
  }

  template <i18n::Key K, DiagnosticId Id, typename... Args>
  u32 emit(Id id, Span primary, Args&&... args) {
    const MessageMeta meta = MESSAGE_META[static_cast<u16>(K)];
    return emit_with<K>(meta.severity, meta.stage, static_cast<u8>(id), primary,
                        true, std::forward<Args>(args)...);
  }

  // The same, naming the check the message came from. Only a lint has a
  // name to give: a rule is addressed by it in configuration and docs,
  // and the code a rule also carries is what a machine matches. The name
  // is not copied, so it must outlive the bag; every rule names itself
  // with a string literal.
  // A lint's severity is its configuration's, not the catalog's: a rule
  // the manifest may promote or silence keeps that choice at the site.
  template <i18n::Key K, DiagnosticId Id, typename... Args>
  u32 emit_named(Severity severity,
                 Id id,
                 std::string_view check_name,
                 Span primary,
                 Args&&... args) {
    fmt::memory_buffer out;
    i18n::format_to<K>(std::back_inserter(out), language_,
                       std::forward<Args>(args)...);
    const MessageMeta meta = MESSAGE_META[static_cast<u16>(K)];
    const u32 index = push(severity, Code{meta.stage, static_cast<u8>(id)},
                           primary, true, {out.data(), out.size()});
    if (index < size_) {
      entries_[index].check_name = check_name;
    }
    return index;
  }

  // Appends a diagnostic whose message is composed here rather than from
  // the catalog. Nothing the compiler reports arrives this way: a message
  // a user reads is a catalog entry, and a test that needs a wording of
  // its own inventing one is what is left.
  template <typename... Args>
  u32 emit_untranslated(Severity severity,
                        Stage stage,
                        u8 id,
                        fmt::format_string<Args...> format,
                        Args&&... args) {
    fmt::memory_buffer out;
    fmt::format_to(std::back_inserter(out), format,
                   std::forward<Args>(args)...);
    return push(severity, Code{stage, id}, {}, false, {out.data(), out.size()});
  }

  template <typename... Args>
  u32 emit_untranslated(Severity severity,
                        Stage stage,
                        u8 id,
                        Span primary,
                        fmt::format_string<Args...> format,
                        Args&&... args) {
    fmt::memory_buffer out;
    fmt::format_to(std::back_inserter(out), format,
                   std::forward<Args>(args)...);
    return push(severity, Code{stage, id}, primary, true,
                {out.data(), out.size()});
  }

  // Appends a message from outside any check area: no code, no span, so
  // the severity and the text are all a reader has. This is the bag's
  // side of diag::message, for a command that reports several such
  // complaints and has no single failure to hand the envelope. The text
  // is composed from the catalog by the caller and copied here, so it
  // may be built anywhere.
  u32 emit_message(Severity severity, std::string_view message) {
    return push(severity, std::nullopt, {}, false, message);
  }

  // Attaches secondary labels to a previously emitted diagnostic. The labels
  // array is copied into the arena. Fails when `index` does not name an
  // emitted diagnostic. The label messages are not copied, so they must
  // outlive the bag; a composed one goes through label_composed.
  base::Result<void, BagError> label(u32 index,
                                     std::initializer_list<Label> labels);

  // The same, for a message composed at the call site: the bytes are
  // copied into the bag's arena first, so the label outlives the buffer
  // that produced it. A lint's suggestion is the caller this exists for.
  base::Result<void, BagError> label_composed(u32 index,
                                              Span span,
                                              std::string_view message);

  // Checked lookup: returns nullptr when `index` does not name an emitted
  // diagnostic.
  const Diagnostic* at(u32 index) const {
    if (index >= size_) {
      return nullptr;
    }
    return &entries_[index];
  }

  // Whether anything observed was an error. A diagnostic the arena could
  // not hold still counts, so an input that broke the compiler's memory
  // budget fails the run rather than passing quietly.
  bool has_errors() const { return error_count_ > 0 || dropped_errors_ > 0; }
  u32 error_count() const { return error_count_; }
  u32 warning_count() const { return warning_count_; }
  u32 size() const { return size_; }

  // Diagnostics observed but not stored, because the arena reserved for
  // them was spent. They carry no message and no span, so a caller can
  // only say how many there were; the run's exit code already accounts
  // for their severities.
  u32 dropped_count() const { return dropped_count_; }

  // The language its messages were composed in, which a caller building
  // another bag over the same input needs to agree on.
  [[nodiscard]] i18n::Language language() const noexcept { return language_; }

  // Appends every diagnostic of `other`, in order, copying the messages and
  // labels into this bag's arena. The two share nothing afterwards, so the
  // one merged from can be read no further.
  //
  // This is what lets a run's diagnostics read the same however many threads
  // read the input: each thread collects into a bag of its own, and the bags
  // are merged in the order the work was listed.
  void merge(const DiagBag& other);

  // Iteration for renderers. Diagnostics are stored newest-last.
  template <typename F>
  void for_each(F&& f) const {
    for (u32 i = 0; i < size_; ++i) {
      f(entries_[i]);
    }
  }

  // Removes exact-duplicate diagnostics, keeping the first of each.
  // Several targets analyze shared modules, so one problem can be
  // reported once per tree; the run renders it once. Error and warning
  // counts follow what remains. Call once analysis is done: indices
  // handed out before are renumbered by the compaction.
  void dedup();

  // Drops every diagnostic at or after `size`. What a phase reported
  // before it was abandoned described a structure that was never
  // finished, so a caller that refuses the input wholesale removes them
  // and says the one thing that happened.
  void truncate(u32 size);

 private:
  // Appends a diagnostic with an already-composed message. Copies the
  // message into the arena; grows the entries array as needed. Returns the
  // stable index of the new entry. The code stays optional: a message
  // from outside a check area has none, and inventing one would render a
  // code nobody allocated.
  u32 push(Severity severity,
           std::optional<Code> code,
           Span primary,
           bool has_primary,
           std::string_view message);

  // Composes K's message and appends it: the one body the public
  // emitters share, with the primary span and its flag spelled out.
  template <i18n::Key K, typename... Args>
  u32 emit_with(Severity severity,
                Stage stage,
                u8 id,
                Span primary,
                bool has_primary,
                Args&&... args) {
    fmt::memory_buffer out;
    i18n::format_to<K>(std::back_inserter(out), language_,
                       std::forward<Args>(args)...);
    return push(severity, Code{stage, id}, primary, has_primary,
                {out.data(), out.size()});
  }

  // Copies bytes into the arena and returns a view of the copy. The
  // caller has checked that they fit.
  std::string_view intern(std::string_view bytes) const;

  // Counts a diagnostic the arena could not hold and answers the index
  // the caller would have received. Severity still decides the run's
  // fate, so a dropped error cannot pass unnoticed.
  u32 drop(Severity severity);

  // The capacity the entries array grows to next, saturating rather than
  // wrapping.
  [[nodiscard]] u32 grown_capacity() const;

  // Whether an allocation of `bytes` at `align` is free, padding
  // included: the arena rounds its cursor up to the alignment before it
  // bumps, so a check that skips the padding can admit a diagnostic
  // whose message then does not fit. Every allocation is decided here
  // first, because the arena reports exhaustion by trapping in debug and
  // failing in release, and its callers may not be in a position to
  // unwind.
  [[nodiscard]] bool has_room_for(usize bytes, usize align) const;

  mem::Arena* arena_;
  i18n::Language language_;
  // Arena-owned array of all diagnostics, in emission order.
  Diagnostic* entries_ = nullptr;
  u32 capacity_ = 0;
  u32 size_ = 0;
  u32 error_count_ = 0;
  u32 warning_count_ = 0;
  u32 dropped_count_ = 0;
  u32 dropped_errors_ = 0;

  static constexpr u32 INITIAL_CAPACITY = 8;

  // A message is composed from the input, and an input can be anything;
  // a quoted token longer than this is clipped with an ellipsis rather
  // than spending the arena on bytes no reader will finish.
  static constexpr usize MAX_MESSAGE_BYTES = 1u << 10;
};

}  // namespace diag
