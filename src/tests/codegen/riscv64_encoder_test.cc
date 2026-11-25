// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <initializer_list>
#include <span>

#include "codegen/riscv64/encoder.h"
#include "codegen/riscv64/isa.h"
#include "codegen/riscv64/minst.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"

namespace codegen::riscv64 {

namespace {

bool equals(std::span<const u8> got, std::initializer_list<u8> want) {
  return got.size() == want.size() &&
         std::equal(got.begin(), got.end(), want.begin());
}

Encoded encode_one(const MInst& inst) {
  Body body;
  body.insts.push_back(inst);
  return encode(body);
}

}  // namespace

TEST_CASE("srai keeps funct7 in the immediate's high bits") {
  // 4202d293: srai t0, t0, 32. Without funct7 this decodes as srli.
  const Encoded encoded = encode_one(
      make_inst(MOp::Srai, 8, Operand{T0}, Operand{T0}, Operand{Imm{32}}));
  CHECK(equals(encoded.bytes, {0x93, 0xd2, 0x02, 0x42}));
}

TEST_CASE("a six-bit shift amount survives the immediate form") {
  // 0202d293: srli t0, t0, 32 needs bit 25 of the immediate field.
  const Encoded encoded = encode_one(
      make_inst(MOp::Srli, 8, Operand{T0}, Operand{T0}, Operand{Imm{32}}));
  CHECK(equals(encoded.bytes, {0x93, 0xd2, 0x02, 0x02}));
}

TEST_CASE("unsigned loads use the zero-extending funct3") {
  const Encoded byte = encode_one(make_inst(
      MOp::Lbu, 1, Operand{T0}, Operand{Mem{.base = RegRef{T1}, .disp = 0}}));
  CHECK(equals(byte.bytes, {0x83, 0x42, 0x03, 0x00}));
  const Encoded word = encode_one(make_inst(
      MOp::Lwu, 4, Operand{T0}, Operand{Mem{.base = RegRef{T1}, .disp = 0}}));
  CHECK(equals(word.bytes, {0x83, 0x62, 0x03, 0x00}));
}

TEST_CASE("integer to double conversions use the double format") {
  const Encoded from_long =
      encode_one(make_inst(MOp::Fcvt, 8, Operand{Reg::F0}, Operand{RA},
                           Operand{FcvtMode::I64ToF64}));
  CHECK(equals(from_long.bytes, {0x53, 0xf0, 0x20, 0xd2}));
  const Encoded from_word =
      encode_one(make_inst(MOp::Fcvt, 8, Operand{Reg::F0}, Operand{RA},
                           Operand{FcvtMode::I32ToF64}));
  CHECK(equals(from_word.bytes, {0x53, 0x80, 0x00, 0xd2}));
}

}  // namespace codegen::riscv64
