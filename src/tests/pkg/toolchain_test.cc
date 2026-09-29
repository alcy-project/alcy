// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/toolchain.h"

#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "source/source.h"

namespace pkg {

namespace {

struct Fixture {
  mem::Arena arena;
  diag::DiagBag bag{arena};

  Fixture() { arena.reserve(1u << 20); }
};

base::Result<Toolchain, diag::Reported> parse(Fixture& f,
                                              std::string_view bytes) {
  return parse_toolchain(bytes, ".alcy/toolchain.toml", source::UNKNOWN_FILE,
                         f.bag, f.arena);
}

}  // namespace

TEST_CASE("Toolchain parses a linker driver") {
  Fixture f;
  base::Result<Toolchain, diag::Reported> result =
      parse(f, "linker = \"lld\"\n");
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  CHECK(std::move(result).unwrap().linker == "lld");
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Toolchain without a linker means defaults") {
  Fixture f;
  base::Result<Toolchain, diag::Reported> empty = parse(f, "");
  CHECK(empty.is_ok());
  if (!empty.is_ok()) {
    return;
  }
  CHECK(std::move(empty).unwrap().linker.empty());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Toolchain ignores unknown keys") {
  Fixture f;
  base::Result<Toolchain, diag::Reported> result =
      parse(f, "linker = \"lld\"\nfrobnicator = 3\n");
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  CHECK(std::move(result).unwrap().linker == "lld");
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Toolchain rejects a mistyped linker") {
  Fixture f;
  CHECK(parse(f, "linker = 3\n").is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Toolchain parses link arguments in order") {
  Fixture f;
  base::Result<Toolchain, diag::Reported> result =
      parse(f, "link-args = [\"-lm\", \"-pthread\"]\n");
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const Toolchain tool = std::move(result).unwrap();
  CHECK(tool.link_args.size() == 2);
  if (tool.link_args.size() != 2) {
    return;
  }
  CHECK(tool.link_args[0] == "-lm");
  CHECK(tool.link_args[1] == "-pthread");
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Toolchain without link arguments means an empty list") {
  Fixture f;
  base::Result<Toolchain, diag::Reported> result =
      parse(f, "linker = \"lld\"\n");
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  CHECK(std::move(result).unwrap().link_args.empty());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Toolchain rejects link arguments that are not a list") {
  Fixture f;
  CHECK(parse(f, "link-args = \"-lm\"\n").is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Toolchain rejects a link argument that is not text") {
  Fixture f;
  CHECK(parse(f, "link-args = [\"-lm\", 3]\n").is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Toolchain rejects broken TOML") {
  Fixture f;
  CHECK(parse(f, "[[unclosed\n").is_err());
  CHECK(f.bag.has_errors());
}

}  // namespace pkg
