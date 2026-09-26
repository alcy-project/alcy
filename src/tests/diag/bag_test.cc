// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/bag.h"

#include "diag/diagnostic.h"
#include "diag/span.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"

namespace diag {

namespace {

struct BagFixture {
  mem::Arena arena;
  DiagBag bag{arena};

  BagFixture() { arena.reserve(1u << 20); }
};

}  // namespace

TEST_CASE("DiagBag counts and iteration") {
  BagFixture f;
  CHECK(!f.bag.has_errors());
  CHECK(f.bag.size() == 0);

  const u32 e = f.bag.emit(Severity::Error, 7, "broken {}", "thing");
  const u32 w = f.bag.emit(Severity::Warning, 8, "shaky");
  f.bag.emit(Severity::Note, 9, "fyi");

  CHECK(f.bag.size() == 3);
  CHECK(f.bag.has_errors());
  CHECK(f.bag.error_count() == 1);
  CHECK(f.bag.warning_count() == 1);
  CHECK(f.bag.at(e)->code == 7);
  CHECK(f.bag.at(e)->message == "broken thing");
  CHECK(f.bag.at(w)->severity == Severity::Warning);

  u32 seen = 0;
  f.bag.for_each([&](const Diagnostic&) { ++seen; });
  CHECK(seen == 3);
}

TEST_CASE("DiagBag spans and labels") {
  BagFixture f;
  const Span span{.file = 3, .offset = 8, .length = 3};
  const u32 i =
      f.bag.emit(Severity::Error, 1, span, "bad call from {}", "here");
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

}  // namespace diag
