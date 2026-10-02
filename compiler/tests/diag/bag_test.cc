// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/bag.h"

#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
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

}  // namespace diag
