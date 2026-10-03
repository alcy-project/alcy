// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/bag.h"

#include <string>

#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "fpag/mem/page_allocator.h"
#include "i18n/language.h"

namespace diag {

namespace {

struct BagFixture {
  mem::Arena arena;
  DiagBag bag{arena, i18n::Language::EnUs};

  BagFixture() { arena.reserve(1u << 20); }
};

}  // namespace

TEST_CASE("DiagBag counts and iteration") {
  BagFixture f;
  CHECK(!f.bag.has_errors());
  CHECK(f.bag.size() == 0);

  const u32 e = f.bag.emit_untranslated(Severity::Error, Stage::Lexer, 7,
                                        "broken {}", "thing");
  const u32 w =
      f.bag.emit_untranslated(Severity::Warning, Stage::Lexer, 8, "shaky");
  f.bag.emit_untranslated(Severity::Note, Stage::Lexer, 9, "fyi");

  CHECK(f.bag.size() == 3);
  CHECK(f.bag.has_errors());
  CHECK(f.bag.error_count() == 1);
  CHECK(f.bag.warning_count() == 1);
  const Diagnostic* const reported = f.bag.at(e);
  CHECK(reported->code.has_value());
  if (!reported->code.has_value()) {
    return;
  }
  CHECK(reported->code->id == 7);
  CHECK(reported->code->stage == Stage::Lexer);
  CHECK(reported->message == "broken thing");
  CHECK(f.bag.at(w)->severity == Severity::Warning);

  u32 seen = 0;
  f.bag.for_each([&](const Diagnostic&) { ++seen; });
  CHECK(seen == 3);
}

TEST_CASE("DiagBag spans and labels") {
  BagFixture f;
  const Span span{.file = 3, .offset = 8, .length = 3};
  const u32 i = f.bag.emit_untranslated(Severity::Error, Stage::Lexer, 1, span,
                                        "bad call from {}", "here");
  CHECK(f.bag.at(i)->has_primary_span);
  CHECK(f.bag.at(i)->primary_span.offset == 8);
  CHECK(f.bag.at(i)->label_count == 0);

  CHECK(f.bag.label(i, {{{.file = 3, .offset = 23, .length = 1}, "used here"}})
            .is_ok());
  CHECK(f.bag.at(i)->label_count == 1);
  CHECK(f.bag.at(i)->labels[0].message == "used here");
}

TEST_CASE("Reported result smoke test") {
  base::Result<i32, diag::Reported> ok = base::make_ok(3);
  CHECK(ok.is_ok());
  CHECK(!ok.is_err());

  base::Result<i32, diag::Reported> err = base::make_err(Reported{});
  CHECK(err.is_err());
  CHECK(!err.is_ok());
}

TEST_CASE("A spent diagnostic arena drops rather than crashes") {
  // The arena is the caller's, and a caller that reserved too little
  // used to write through nothing in release and trap in debug. What it
  // gives up now is the diagnostics, counted and reported.
  mem::Arena arena;
  arena.reserve(mem::page_size());
  DiagBag bag{arena, i18n::Language::EnUs};
  constexpr u32 ATTEMPTS = 1000;
  for (u32 i = 0; i < ATTEMPTS; ++i) {
    bag.emit_untranslated(Severity::Error, Stage::Lexer, 7, "no room {}", i);
  }
  CHECK(bag.size() < ATTEMPTS);
  CHECK(bag.dropped_count() == ATTEMPTS - bag.size());
  // Every one was an error, so the run fails whether or not any was
  // stored.
  CHECK(bag.has_errors());
  CHECK(bag.error_count() > 0);
}

TEST_CASE("A message longer than the cap is clipped") {
  mem::Arena arena;
  arena.reserve(mem::page_size());
  DiagBag bag{arena, i18n::Language::EnUs};
  const std::string huge(64u << 10, 'x');
  const u32 index = bag.emit_untranslated(Severity::Error, Stage::Parser, 1,
                                          "found {}", huge);
  CHECK(bag.size() == 1);
  CHECK(bag.dropped_count() == 0);
  const Diagnostic* const stored = bag.at(index);
  CHECK(stored != nullptr);
  if (stored == nullptr) {
    return;
  }
  CHECK(stored->message.size() < huge.size());
  CHECK(stored->message.ends_with("..."));
}

TEST_CASE("DiagBag merge carries dropped diagnostics") {
  mem::Arena small;
  small.reserve(mem::page_size());
  DiagBag from{small, i18n::Language::EnUs};
  for (u32 i = 0; i < 1000; ++i) {
    from.emit_untranslated(Severity::Error, Stage::Lexer, 7, "full {}", i);
  }
  CHECK(from.dropped_count() > 0);

  BagFixture into;
  into.bag.merge(from);
  CHECK(into.bag.dropped_count() == from.dropped_count());
  // The dropped errors do not vanish with their messages.
  CHECK(into.bag.has_errors());
}

TEST_CASE("DiagBag truncate drops the tail and its counts") {
  BagFixture f;
  const u32 first =
      f.bag.emit_untranslated(Severity::Error, Stage::Lexer, 1, "first");
  f.bag.emit_untranslated(Severity::Warning, Stage::Lexer, 2, "second");
  f.bag.emit_untranslated(Severity::Error, Stage::Lexer, 3, "third");
  CHECK(f.bag.size() == 3);
  f.bag.truncate(first + 1);
  CHECK(f.bag.size() == 1);
  CHECK(f.bag.error_count() == 1);
  CHECK(f.bag.warning_count() == 0);
  CHECK(f.bag.at(0)->message == "first");
  // A size the bag never reached changes nothing.
  f.bag.truncate(99);
  CHECK(f.bag.size() == 1);
  CHECK(f.bag.error_count() == 1);
}

TEST_CASE("DiagBag dedup keeps the first of each") {
  BagFixture f;
  const Span span{.file = 1, .offset = 2, .length = 3};
  f.bag.emit_untranslated(Severity::Warning, Stage::Lowering, 4, span, "twice");
  f.bag.emit_untranslated(Severity::Warning, Stage::Lowering, 4, span, "twice");
  f.bag.emit_untranslated(Severity::Warning, Stage::Lowering, 4, span,
                          "different");
  f.bag.emit_untranslated(Severity::Warning, Stage::Lowering, 4,
                          Span{.file = 1, .offset = 9, .length = 3}, "twice");
  f.bag.emit_untranslated(Severity::Error, Stage::Lowering, 4, span, "twice");
  CHECK(f.bag.size() == 5);
  f.bag.dedup();
  CHECK(f.bag.size() == 4);
  CHECK(f.bag.warning_count() == 3);
  CHECK(f.bag.error_count() == 1);
}

TEST_CASE("DiagBag dedup labels are part of identity") {
  BagFixture f;
  const Span span{.file = 1, .offset = 2, .length = 3};
  const Label label{.span = {.file = 1, .offset = 5, .length = 1},
                    .message = "here"};
  const u32 first = f.bag.emit_untranslated(Severity::Warning, Stage::Borrow, 4,
                                            span, "borrowed");
  CHECK(f.bag.label(first, {label}).is_ok());
  // The same message and label from a second tree is the same problem.
  const u32 again = f.bag.emit_untranslated(Severity::Warning, Stage::Borrow, 4,
                                            span, "borrowed");
  CHECK(f.bag.label(again, {label}).is_ok());
  // One without the label renders differently, so it stands apart.
  f.bag.emit_untranslated(Severity::Warning, Stage::Borrow, 4, span,
                          "borrowed");
  CHECK(f.bag.size() == 3);
  f.bag.dedup();
  CHECK(f.bag.size() == 2);
  CHECK(f.bag.warning_count() == 2);
  CHECK(f.bag.at(0)->label_count == 1);
}

TEST_CASE("DiagBag merge keeps every entry whole") {
  BagFixture from;
  BagFixture into;
  const Span span{.file = 2, .offset = 4, .length = 5};
  const u32 e = from.bag.emit_untranslated(Severity::Error, Stage::Parser, 3,
                                           span, "bad {}", "node");
  CHECK(from.bag.label(e, {{{.file = 2, .offset = 9, .length = 1}, "here"}})
            .is_ok());
  from.bag.emit_untranslated(Severity::Warning, Stage::Lexer, 8, "shaky");

  into.bag.merge(from.bag);
  CHECK(into.bag.size() == 2);
  CHECK(into.bag.error_count() == 1);
  CHECK(into.bag.warning_count() == 1);
  const Diagnostic* const merged = into.bag.at(0);
  CHECK(merged != nullptr);
  if (merged == nullptr) {
    return;
  }
  // The code crosses the merge rather than being reissued: a message
  // that had none must keep having none, and one that had one must
  // keep that exact check.
  CHECK(merged->code.has_value());
  if (!merged->code.has_value()) {
    return;
  }
  CHECK(merged->code->stage == Stage::Parser);
  CHECK(merged->code->id == 3);
  CHECK(merged->message == "bad node");
  CHECK(merged->has_primary_span);
  CHECK(merged->primary_span.offset == 4);
  CHECK(merged->label_count == 1);
  CHECK(merged->labels[0].message == "here");
}

// The arena rounds its cursor up to an allocation's alignment before it
// bumps, so the room check has to count that padding as part of the
// request. This case lands in the sliver where the old check passed and
// the message then did not fit: the entries table had grown twice, and
// the next push asks for the following table plus a one-byte message
// with exactly enough room for the two but not for the byte of padding
// in front of the table.
TEST_CASE("A message that misses the alignment padding is dropped") {
  mem::Arena arena;
  arena.reserve(mem::page_size());
  DiagBag bag{arena, i18n::Language::EnUs};
  constexpr usize MESSAGE_CAP = 1u << 10;
  const usize entry_bytes = sizeof(Diagnostic);
  // N messages fit before the push that grows the table to 2N entries,
  // leaving a message for each. The tables before that push are the
  // grown 8, 16, ... up to N, which sum to 2N - 8 entries; the push
  // itself asks for 2N more. One byte is left for the message that
  // trips the padding.
  usize messages = 16;
  usize fill = 0;
  while (messages <= 64) {
    const usize tables = (4 * messages - 8) * entry_bytes + 1;
    if (arena.capacity() > tables) {
      fill = arena.capacity() - tables;
      if (fill <= messages * MESSAGE_CAP) {
        break;
      }
    }
    messages *= 2;
  }
  // A page large enough to need more than sixty-four messages has not
  // been calibrated; no host this compiler builds for has one.
  if (messages > 64) {
    return;
  }
  for (usize i = 0; i < messages; ++i) {
    const usize bytes = i + 1 == messages
                            ? fill - (messages - 1) * (fill / messages)
                            : fill / messages;
    bag.emit_untranslated(Severity::Error, Stage::Lexer, 7, "{}",
                          std::string(bytes, 'x'));
  }
  CHECK(bag.size() == messages);
  bag.emit_untranslated(Severity::Error, Stage::Lexer, 7, "{}",
                        std::string(1, 'y'));
  CHECK(bag.size() == messages);
  CHECK(bag.dropped_count() == 1);
  CHECK(bag.has_errors());
  // Nothing stored can have lost its message to the padding.
  bag.for_each(
      [](const Diagnostic& stored) { CHECK(!stored.message.empty()); });
}

}  // namespace diag
