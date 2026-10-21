// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/worker_bags.h"

#include <string>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"

namespace {

using diag::DiagBag;
using diag::Severity;
using diag::Stage;

// What one bag said, in the order it said it.
std::vector<std::string> messages_of(const DiagBag& bag) {
  std::vector<std::string> said;
  bag.for_each([&](const diag::Diagnostic& diagnostic) {
    said.emplace_back(diagnostic.message);
  });
  return said;
}

TEST_CASE(
    "Worker bags merge in unit order, not in the order they were filled") {
  diag::WorkerBags bags{i18n::Language::EnUs, /*workers=*/2, /*units=*/3};
  // The units are filled last first, the way two workers finishing out of
  // order would, and each unit keeps its own message.
  bags.bag_for(2, 1).emit_untranslated(Severity::Error, Stage::Pipeline, 3,
                                       "third");
  bags.bag_for(1, 0).emit_untranslated(Severity::Error, Stage::Pipeline, 2,
                                       "second");
  bags.bag_for(0, 0).emit_untranslated(Severity::Error, Stage::Pipeline, 1,
                                       "first");

  mem::Arena arena;
  arena.reserve(1u << 20);
  DiagBag into{arena, i18n::Language::EnUs};
  bags.merge_into(into, 3);

  CHECK(messages_of(into) ==
        std::vector<std::string>{"first", "second", "third"});
}

TEST_CASE("A merge covers the units it is asked for") {
  diag::WorkerBags bags{i18n::Language::EnUs, 1, 2};
  bags.bag_for(0, 0).emit_untranslated(Severity::Error, Stage::Pipeline, 1,
                                       "first");
  bags.bag_for(1, 0).emit_untranslated(Severity::Error, Stage::Pipeline, 2,
                                       "second");

  mem::Arena arena;
  arena.reserve(1u << 20);
  DiagBag into{arena, i18n::Language::EnUs};
  bags.merge_into(into, 1);

  CHECK(messages_of(into) == std::vector<std::string>{"first"});
}

TEST_CASE("A unit that reported nothing contributes nothing") {
  diag::WorkerBags bags{i18n::Language::EnUs, 2, 2};
  bags.bag_for(1, 1).emit_untranslated(Severity::Error, Stage::Pipeline, 1,
                                       "only");

  mem::Arena arena;
  arena.reserve(1u << 20);
  DiagBag into{arena, i18n::Language::EnUs};
  bags.merge_into(into, 2);

  CHECK(messages_of(into) == std::vector<std::string>{"only"});
}

}  // namespace
