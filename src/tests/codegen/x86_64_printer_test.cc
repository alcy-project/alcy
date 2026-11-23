// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <string>
#include <vector>

#include "codegen/object/elf64.h"
#include "codegen/x86_64/isa.h"
#include "codegen/x86_64/minst.h"
#include "codegen/x86_64/printer.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"

namespace codegen::x86_64 {
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

TEST_CASE("The printer writes AT&T operands and local labels") {
  Body body;
  body.symbols.push_back("callee");
  MInst first = make_inst(MOp::Mov, 8, Operand{Reg::Rax}, Operand{Imm{7}});
  first.label = 0;
  body.insts.push_back(first);
  body.insts.push_back(
      make_inst(MOp::Add, 8, Operand{Reg::Rbx}, Operand{Reg::Rax}));
  body.insts.push_back(make_inst(MOp::Call, 8, Operand{SymbolRef{0}}));
  body.insts.push_back(make_jcc(Cond::Ne, Operand{Label{0}}));
  body.insts.push_back(make_inst(MOp::Ret, 8));

  object::Object object = text_object(5);
  std::vector<PrintedBody> bodies{
      PrintedBody{"fn", body, object::SymbolBind::Local, {}, 0, 5}};
  const std::string text = print_assembly(object, bodies);
  CHECK(contains(text, "fn:\n"));
  CHECK(contains(text, "\tmovq $7, %rax\n"));
  CHECK(contains(text, "\taddq %rax, %rbx\n"));
  CHECK(contains(text, "\tcall callee\n"));
  CHECK(contains(text, "fn..L0:\n"));
  CHECK(contains(text, "\tjne fn..L0\n"));
  CHECK(contains(text, "\tret\n"));
  CHECK(contains(text, "\t.size fn, .-fn\n"));
}

TEST_CASE("The printer formats a memory operand in AT&T order") {
  Body body;
  const Mem source{
      .base = Reg::Rbp, .index = std::nullopt, .scale = 1, .disp = -8};
  body.insts.push_back(
      make_inst(MOp::Mov, 8, Operand{Reg::Rax}, Operand{source}));
  object::Object object = text_object(1);
  std::vector<PrintedBody> bodies{
      PrintedBody{"fn", body, object::SymbolBind::Local, {}, 0, 1}};
  const std::string text = print_assembly(object, bodies);
  CHECK(contains(text, "\tmovq -8(%rbp), %rax\n"));
}

TEST_CASE("The printer spells a register shift count as cl") {
  Body body;
  body.insts.push_back(
      make_inst(MOp::Mov, 1, Operand{Reg::Rcx}, Operand{Reg::Rax}));
  body.insts.push_back(
      make_inst(MOp::Shl, 4, Operand{Reg::Rax}, Operand{Reg::Rcx}));
  object::Object object = text_object(2);
  std::vector<PrintedBody> bodies{
      PrintedBody{"fn", body, object::SymbolBind::Local, {}, 0, 2}};
  const std::string text = print_assembly(object, bodies);
  CHECK(contains(text, "\tmovb %al, %cl\n"));
  CHECK(contains(text, "\tshll %cl, %eax\n"));
}

TEST_CASE("The printer labels data sections with their symbols") {
  object::Object object = text_object(0);
  object.sections.push_back(object::Section{
      .name = ".rodata",
      .bytes = std::vector<u8>{0xde, 0xad},
      .align = 1,
      .alloc = true,
      .bits = true,
  });
  object.symbols.push_back(object::Symbol{
      .name = "str",
      .section = 1,
      .offset = 0,
      .size = 2,
      .bind = object::SymbolBind::Local,
      .type = object::SymbolType::Object,
  });
  const std::vector<PrintedBody> none;
  const std::string text = print_assembly(object, none);
  CHECK(contains(text, "\t.section .rodata,\"a\",@progbits\n"));
  CHECK(contains(text, "str:\n"));
  CHECK(contains(text, "\t.byte 0xde, 0xad\n"));
}

}  // namespace codegen::x86_64
