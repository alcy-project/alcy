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
  CHECK(equals(from_long.bytes, {0x53, 0x80, 0x20, 0xd2}));
  const Encoded from_word =
      encode_one(make_inst(MOp::Fcvt, 8, Operand{Reg::F0}, Operand{RA},
                           Operand{FcvtMode::I32ToF64}));
  CHECK(equals(from_word.bytes, {0x53, 0x80, 0x00, 0xd2}));
}

TEST_CASE("An external symbol survives as a symbol patch") {
  Body body;
  body.symbols.push_back("foreign");
  body.insts.push_back(
      make_inst(MOp::Addi, 8, Operand{T0}, Operand{T0}, Operand{SymbolRef{0}}));
  const Encoded encoded = encode(body);
  CHECK(encoded.patches.size() == 1);
  CHECK(encoded.patches[0].symbol == "foreign");
  CHECK(encoded.patches[0].offset == 0);
}

TEST_CASE("Every float conversion mode has its own encoding") {
  struct Case {
    FcvtMode mode;
    std::initializer_list<u8> bytes;
  };
  const Case cases[] = {
      {FcvtMode::F64ToI64, {0x53, 0x90, 0x20, 0xc2}},
      {FcvtMode::F32ToI64, {0x53, 0x90, 0x20, 0xc0}},
      {FcvtMode::I64ToF64, {0x53, 0x80, 0x20, 0xd2}},
      {FcvtMode::I32ToF64, {0x53, 0x80, 0x00, 0xd2}},
      {FcvtMode::F64ToIU64, {0x53, 0x90, 0x30, 0xc2}},
      {FcvtMode::F32ToIU64, {0x53, 0x90, 0x30, 0xc0}},
      {FcvtMode::F64ToF32, {0x53, 0x80, 0x10, 0x40}},
      {FcvtMode::F32ToF64, {0x53, 0x80, 0x00, 0x42}},
      {FcvtMode::IU64ToF64, {0x53, 0x80, 0x30, 0xd2}},
      {FcvtMode::IU32ToF64, {0x53, 0x80, 0x10, 0xd2}},
      {FcvtMode::IU64ToF32, {0x53, 0x80, 0x30, 0xd0}},
      {FcvtMode::IU32ToF32, {0x53, 0x80, 0x10, 0xd0}},
      {FcvtMode::I64ToF32, {0x53, 0x80, 0x20, 0xd0}},
      {FcvtMode::I32ToF32, {0x53, 0x80, 0x00, 0xd0}},
  };
  for (const Case& test : cases) {
    const Encoded encoded = encode_one(make_inst(
        MOp::Fcvt, 8, Operand{Reg::F0}, Operand{RA}, Operand{test.mode}));
    CHECK(equals(encoded.bytes, test.bytes));
  }
}

TEST_CASE("stores keep a negative displacement and mulhu uses funct3 3") {
  const Encoded store = encode_one(make_inst(
      MOp::Sd, 8, Operand{T0}, Operand{Mem{.base = RegRef{SP}, .disp = -8}}));
  CHECK(equals(store.bytes, {0x23, 0x3c, 0x51, 0xfe}));
  const Encoded mul = encode_one(
      make_inst(MOp::Mulhu, 8, Operand{T0}, Operand{T1}, Operand{T2}));
  CHECK(equals(mul.bytes, {0xb3, 0x32, 0x73, 0x02}));
}

TEST_CASE("forward and backward label patches use the right displacement") {
  Body body;
  MInst first = make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{1}});
  first.label = 0;
  body.insts.push_back(first);
  body.insts.push_back(make_inst(MOp::Nop, 8));
  MInst third =
      make_inst(MOp::Beq, 8, Operand{T0}, Operand{T1}, Operand{Label{0}});
  third.label = 2;
  body.insts.push_back(third);
  body.insts.push_back(make_inst(MOp::Nop, 8));
  const Encoded encoded = encode(body);
  // j 4: jal zero, +4, and beq t0,t1,-8, both patched to their labels.
  CHECK(equals(std::span<const u8>(encoded.bytes).subspan(0, 4),
               {0x6f, 0x00, 0x00, 0x00}));
  CHECK(equals(std::span<const u8>(encoded.bytes).subspan(8, 4),
               {0xe3, 0x8c, 0x62, 0xfe}));
}

}  // namespace codegen::riscv64
