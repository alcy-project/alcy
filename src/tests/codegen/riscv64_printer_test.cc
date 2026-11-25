// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string>
#include <vector>

#include "codegen/object/elf64.h"
#include "codegen/riscv64/isa.h"
#include "codegen/riscv64/minst.h"
#include "codegen/riscv64/printer.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"

namespace codegen::riscv64 {
namespace {

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

object::Object text_object(const usize size) {
  object::Object object;
  object.sections.push_back(object::Section{
      .name = ".text",
      .bytes = std::vector<u8>(size, 0),
      .align = 16,
      .exec = true,
      .alloc = true,
      .bits = true,
  });
  return object;
}

}  // namespace

TEST_CASE("The riscv64 printer writes fixed-width instructions") {
  Body body;
  body.symbols.push_back("callee");
  MInst first =
      make_inst(MOp::Addi, 8, Operand{A0}, Operand{ZERO}, Operand{Imm{7}});
  first.label = 0;
  body.insts.push_back(first);
  body.insts.push_back(
      make_inst(MOp::Jalr, 8, Operand{RA}, Operand{T0}, Operand{Imm{0}}));
  body.insts.push_back(make_inst(MOp::Sd, 8, Operand{A0},
                                 Operand{Mem{.base = RegRef{SP}, .disp = 8}}));
  body.insts.push_back(
      make_inst(MOp::Beq, 8, Operand{A0}, Operand{ZERO}, Operand{Label{0}}));
  body.insts.push_back(make_inst(MOp::Ecall, 8));

  object::Object object = text_object(body.insts.size() * 4);
  std::vector<PrintedBody> bodies{PrintedBody{
      "fn", body, object::SymbolBind::Local, 0, body.insts.size() * 4}};
  const std::string text = print_assembly(object, bodies);
  CHECK(contains(text, "\t.option norvc\n"));
  CHECK(contains(text, "fn:\n"));
  CHECK(contains(text, "\taddi a0, zero, 7\n"));
  CHECK(contains(text, "\tjalr ra, t0, 0\n"));
  CHECK(contains(text, "\tsd a0, 8(sp)\n"));
  CHECK(contains(text, "\tbeq a0, zero, fn..L0\n"));
  CHECK(contains(text, "\tecall\n"));
  CHECK(contains(text, "\t.size fn, .-fn\n"));
}

TEST_CASE("The riscv64 printer folds immediates into ALU forms") {
  Body body;
  body.insts.push_back(
      make_inst(MOp::And, 8, Operand{T0}, Operand{Imm{32}}, Operand{T1}));
  body.insts.push_back(
      make_inst(MOp::Mulhu, 8, Operand{T0}, Operand{T1}, Operand{T2}));
  body.insts.push_back(
      make_inst(MOp::Fmov, 8, Operand{Reg::F10}, Operand{Reg::F6}));
  body.insts.push_back(make_inst(MOp::Fmul, 8, Operand{Reg::F10},
                                 Operand{Reg::F6}, Operand{Reg::F7}));
  body.insts.push_back(make_inst(MOp::Fcvt, 8, Operand{T0}, Operand{Reg::F6},
                                 Operand{FcvtMode::F64ToI64}));
  object::Object object = text_object(body.insts.size() * 4);
  for (u32 i = 0; i < body.insts.size(); ++i) {
    object.symbols.push_back(object::Symbol{.name = "fn",
                                            .section = 0,
                                            .offset = static_cast<u64>(i) * 4,
                                            .size = 4,
                                            .bind = object::SymbolBind::Local,
                                            .type = object::SymbolType::Func});
  }
  std::vector<PrintedBody> bodies{PrintedBody{
      "fn", body, object::SymbolBind::Local, 0, body.insts.size() * 4}};
  const std::string text = print_assembly(object, bodies);
  CHECK(contains(text, "\tandi t0, t1, 32\n"));
  CHECK(contains(text, "\tmulhu t0, t1, t2\n"));
  CHECK(contains(text, "\tfsgnj.d f10, f6, f6\n"));
  CHECK(contains(text, "\tfmul.d f10, f6, f7, rne\n"));
  CHECK(contains(text, "\tfcvt.l.d t0, f6, rtz\n"));
}

TEST_CASE("The riscv64 printer names a resolved jump target") {
  Body body;
  MInst jump = make_inst(MOp::Jal, 8, Operand{RA}, Operand{Imm{-16}});
  body.insts.push_back(jump);
  object::Object object = text_object(20);
  object.symbols.push_back(object::Symbol{.name = "main",
                                          .section = 0,
                                          .offset = 0,
                                          .size = 4,
                                          .bind = object::SymbolBind::Global,
                                          .type = object::SymbolType::Func});
  object.symbols.push_back(object::Symbol{.name = "fn",
                                          .section = 0,
                                          .offset = 16,
                                          .size = 4,
                                          .bind = object::SymbolBind::Local,
                                          .type = object::SymbolType::Func});
  std::vector<PrintedBody> bodies{
      PrintedBody{"fn", body, object::SymbolBind::Local, 16, 4}};
  const std::string text = print_assembly(object, bodies);
  CHECK(contains(text, "\tjal ra, main\n"));
}

}  // namespace codegen::riscv64
