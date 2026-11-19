// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/build_progress.h"

#include <string>
#include <string_view>

#include "cli/logger.h"
#include "diag/render.h"
#include "doctest/doctest.h"
#include "i18n/language.h"

namespace cli {

namespace {

struct Collector {
  std::string text;

  static void write(void* ctx, std::string_view block) {
    static_cast<Collector*>(ctx)->text += block;
  }
};

}  // namespace

TEST_CASE("A build announces its scope on standard error") {
  Collector err;
  const Logger logger{&Collector::write, &err};
  const BuildProgress progress{
      logger, true,
      diag::RenderOptions{.color = false, .language = i18n::Language::EnUs}};

  progress.begin("hello", 1, false);
  progress.begin("acme/std", 17, true);
  progress.begin("solo", 1, true);

  CHECK(err.text ==
        "Building  package hello (1 target)\n"
        "Building  suite acme/std (17 packages)\n"
        "Building  suite solo (1 package)\n");
}

TEST_CASE("Members that compiled announce with their time, in order") {
  Collector err;
  const Logger logger{&Collector::write, &err};
  const BuildProgress progress{
      logger, true,
      diag::RenderOptions{.color = false, .language = i18n::Language::EnUs}};

  progress.member_done("core", 2100000000ull, false);
  progress.member_done("arch", 210000000ull, false);
  // A member the cache answered from is left out of the lines.
  progress.member_done("quiet", 1000000ull, true);

  CHECK(err.text == "  core (2.10 s)\n  arch (210 ms)\n");
}

TEST_CASE("A member announcement is dimmed, with the newline last") {
  Collector err;
  const Logger logger{&Collector::write, &err};
  const BuildProgress progress{
      logger, true,
      diag::RenderOptions{.color = true, .language = i18n::Language::EnUs}};

  // The colour run must close with RESET before the newline, or the
  // Logger's block contract (a single trailing newline) does not hold.
  progress.member_done("core", 2100000000ull, false);

  CHECK(err.text == "\033[2m  core (2.10 s)\033[0m\n");
}

TEST_CASE("A begin announcement is dimmed, with the newline last") {
  Collector err;
  const Logger logger{&Collector::write, &err};
  const BuildProgress progress{
      logger, true,
      diag::RenderOptions{.color = true, .language = i18n::Language::EnUs}};

  progress.begin("hello", 2, false);

  CHECK(err.text ==
        "\033[1m\033[92mBuilding\033[0m  "
        "\033[4mpackage hello (2 targets)\033[0m\n");
}

TEST_CASE("A disabled build prints nothing") {
  Collector err;
  const Logger logger{&Collector::write, &err};
  const BuildProgress progress{
      logger, false,
      diag::RenderOptions{.color = false, .language = i18n::Language::EnUs}};

  progress.begin("hello", 1, false);
  progress.member_done("core", 2100000000ull, false);

  CHECK(err.text.empty());
}

}  // namespace cli
