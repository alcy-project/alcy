// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "base/hash.h"

#include <array>
#include <string_view>

#include "doctest/doctest.h"

namespace base {

namespace {

// The vectors are `xxhsum` 0.8.4 (the vendored revision) at seed 0, so
// a drift in the algorithm or the wiring is a test failure rather than
// a silent change of every recorded digest.

TEST_CASE("hash64 answers the published vectors") {
  CHECK(hash64("") == 0x2d06800538d394c2ull);
  CHECK(hash64("hello") == 0x9555e8555c62dcfdull);
  CHECK(hash64("abc") == 0x78af5f94892f3950ull);
}

TEST_CASE("hash128 answers the published vectors") {
  CHECK(hash128("") == Digest128{0x6001c324468d497full, 0x99aa06d3014798d8ull});
  CHECK(hash128("hello") ==
        Digest128{0xc779cfaa5e523818ull, 0xb5e9c1ad071b3e7full});
}

TEST_CASE("Digests hash deterministically and apart") {
  CHECK(hash64("hello") == hash64(std::string_view("hello")));
  CHECK(hash64("hello") != hash64("hellp"));
  CHECK(hash128("hello") == hash128("hello"));
  CHECK(hash128("hello") != hash128("hellp"));
  CHECK(hash128("") != Digest128{});
}

TEST_CASE("Digest128 reads back its bytes and hex") {
  const Digest128 digest = hash128("");
  const std::array<u8, 16> bytes = digest.bytes();
  u64 low = 0;
  u64 high = 0;
  for (u32 i = 0; i < 8; ++i) {
    low |= static_cast<u64>(bytes[i]) << (8 * i);
    high |= static_cast<u64>(bytes[8 + i]) << (8 * i);
  }
  CHECK(low == digest.low);
  CHECK(high == digest.high);
  CHECK(digest.hex() == "99aa06d3014798d86001c324468d497f");
}

}  // namespace

}  // namespace base
