// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "fingerprint/fingerprint.h"

#include <string_view>

#include "base/hash.h"
#include "doctest/doctest.h"

namespace fingerprint {

TEST_CASE("The same fields in the same order reach one digest") {
  Builder first;
  first.bytes("path").u8(0).digest(base::hash128("body"));
  Builder second;
  second.bytes("path").u8(0).digest(base::hash128("body"));
  CHECK(first.finish() == second.finish());
}

TEST_CASE("Field order and an explicit delimiter change the digest") {
  Builder joined;
  joined.bytes("ab").bytes("c");
  Builder delimited;
  delimited.bytes("ab").u8(0).bytes("c");
  CHECK(joined.finish() != delimited.finish());
  Builder other_order;
  other_order.bytes("c").bytes("ab");
  CHECK(joined.finish() != other_order.finish());
}

TEST_CASE("A digest is appended in the IR's little-endian order") {
  const Digest value{0x0102030405060708ull, 0x1112131415161718ull};
  Builder append;
  append.digest(value);
  const auto bytes = value.bytes();
  Builder raw;
  raw.bytes(std::string_view(reinterpret_cast<const char*>(bytes.data()),
                             bytes.size()));
  CHECK(append.finish() == raw.finish());
}

}  // namespace fingerprint
