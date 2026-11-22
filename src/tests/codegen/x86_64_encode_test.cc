// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <span>

#include "codegen/x86_64/encode.h"
#include "codegen/x86_64/isa.h"
#include "codegen/x86_64/minst.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"

namespace codegen::x86_64 {

namespace {

bool equals(std::span<const u8> got, std::initializer_list<u8> want) {
  return got.size() == want.size() &&
         std::equal(got.begin(), got.end(), want.begin());
}

}  // namespace

TEST_CASE("setcc names an extended byte register with REX.B") {
  Body body;
  body.insts.push_back(make_setcc(Cond::E, Operand{Reg::R10}));
  const Encoded encoded = encode(body);
  // 41 0f 94 r/m: REX.B, sete, ModRM rm=r10 (code 2).
  CHECK(equals(encoded.bytes, {0x41, 0x0f, 0x94, 0xc2}));
}

TEST_CASE("a 16-bit ALU immediate takes exactly two bytes") {
  Body body;
  body.insts.push_back(
      make_inst(MOp::Add, 2, Operand{Reg::Rax}, Operand{Imm{300}}));
  const Encoded encoded = encode(body);
  // 66 81 c0 2c 01: operand-size prefix, add, imm16 = 300.
  CHECK(equals(encoded.bytes, {0x66, 0x81, 0xc0, 0x2c, 0x01}));
}

TEST_CASE("movzx through an extended base keeps REX.B") {
  Body body;
  const Mem from{
      .base = Reg::R10, .index = std::nullopt, .scale = 1, .disp = 0};
  body.insts.push_back(
      make_inst(MOp::Movzx, 1, Operand{Reg::Rax}, Operand{from}));
  const Encoded encoded = encode(body);
  // 49 0f b6 02: REX.B, movzx rax, byte ptr [r10].
  CHECK(equals(encoded.bytes, {0x49, 0x0f, 0xb6, 0x02}));
}

TEST_CASE("call and indirect call keep their expected lengths") {
  {
    Body body;
    body.symbols.push_back("callee");
    body.insts.push_back(make_inst(MOp::Call, 8, Operand{SymbolRef{0}}));
    const Encoded encoded = encode(body);
    CHECK(encoded.bytes.size() == 5);
    CHECK(encoded.bytes[0] == 0xe8);
    CHECK(encoded.patches.size() == 1);
    if (!encoded.patches.empty()) {
      CHECK(encoded.patches[0].addend == -4);
    }
  }
  {
    Body body;
    body.insts.push_back(make_inst(MOp::CallReg, 8, Operand{Reg::Rax}));
    const Encoded encoded = encode(body);
    // ff d0: call rax, no REX needed.
    CHECK(equals(encoded.bytes, {0xff, 0xd0}));
  }
}

}  // namespace codegen::x86_64
