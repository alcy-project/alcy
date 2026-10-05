// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <initializer_list>
#include <span>
#include <vector>

#include "codegen/wasm/writer.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"

namespace codegen::wasm {

namespace {

bool equals(std::span<const u8> got, std::initializer_list<u8> want) {
  return got.size() == want.size() &&
         std::equal(got.begin(), got.end(), want.begin());
}

}  // namespace

TEST_CASE("Unsigned LEB128 matches the format's examples") {
  {
    BinaryWriter writer;
    writer.u32_leb(0);
    CHECK(equals(writer.bytes(), {0x00}));
  }
  {
    BinaryWriter writer;
    writer.u32_leb(127);
    CHECK(equals(writer.bytes(), {0x7F}));
  }
  {
    BinaryWriter writer;
    writer.u32_leb(128);
    CHECK(equals(writer.bytes(), {0x80, 0x01}));
  }
  {
    BinaryWriter writer;
    writer.u32_leb(624485);
    CHECK(equals(writer.bytes(), {0xE5, 0x8E, 0x26}));
  }
  {
    BinaryWriter writer;
    writer.u64_leb(0x1'0000'0000ULL);
    CHECK(equals(writer.bytes(), {0x80, 0x80, 0x80, 0x80, 0x10}));
  }
}

TEST_CASE("Signed LEB128 keeps negatives in their short form") {
  {
    BinaryWriter writer;
    writer.i32_leb(0);
    CHECK(equals(writer.bytes(), {0x00}));
  }
  {
    BinaryWriter writer;
    writer.i32_leb(-1);
    CHECK(equals(writer.bytes(), {0x7F}));
  }
  {
    BinaryWriter writer;
    writer.i32_leb(63);
    CHECK(equals(writer.bytes(), {0x3F}));
  }
  {
    BinaryWriter writer;
    writer.i32_leb(64);
    CHECK(equals(writer.bytes(), {0xC0, 0x00}));
  }
  {
    BinaryWriter writer;
    writer.i32_leb(-64);
    CHECK(equals(writer.bytes(), {0x40}));
  }
  {
    BinaryWriter writer;
    writer.i32_leb(-65);
    CHECK(equals(writer.bytes(), {0xBF, 0x7F}));
  }
  {
    BinaryWriter writer;
    writer.i32_leb(-123456);
    CHECK(equals(writer.bytes(), {0xC0, 0xBB, 0x78}));
  }
}

TEST_CASE("Names carry their length") {
  BinaryWriter writer;
  writer.name("alcy");
  const std::vector<u8> bytes(writer.bytes().begin(), writer.bytes().end());
  CHECK(bytes.size() == 5);
  CHECK(bytes[0] == 0x04);
  CHECK(bytes[1] == 'a');
  CHECK(bytes[4] == 'y');
}

TEST_CASE("A section is its id and the payload's length") {
  {
    BinaryWriter payload;
    payload.byte(0x60);
    payload.byte(0x00);
    BinaryWriter out;
    write_section(out, 1, payload);
    CHECK(equals(out.bytes(), {0x01, 0x02, 0x60, 0x00}));
  }
  {
    // A payload past 127 bytes gets a two-byte length, which is why the
    // length is written from the payload rather than patched in place.
    BinaryWriter payload;
    for (u32 i = 0; i < 130; ++i) {
      payload.byte(0x41);
    }
    BinaryWriter out;
    write_section(out, 10, payload);
    const std::span<const u8> bytes = out.bytes();
    CHECK(bytes[0] == 0x0A);
    CHECK(bytes[1] == 0x82);
    CHECK(bytes[2] == 0x01);
    CHECK(bytes.size() == 3 + 130);
  }
}

TEST_CASE("Floats are written little-endian") {
  {
    BinaryWriter writer;
    writer.f32_bits(1.0f);
    CHECK(equals(writer.bytes(), {0x00, 0x00, 0x80, 0x3F}));
  }
  {
    BinaryWriter writer;
    writer.f64_bits(1.0);
    CHECK(equals(writer.bytes(),
                 {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F}));
  }
}

}  // namespace codegen::wasm
