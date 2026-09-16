// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "codegen_llvm/llvm_ir_emitter.h"

#include <memory>
#include <string>
#include <utility>

#include "codegen_llvm/common.h"
#include "doctest/doctest.h"
#include "fpag/base/idx.h"
#include "fpag/mem/page_allocator.h"
#include "fpag/str/string_interner.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/opcode.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "tests/util/test_util.h"

namespace codegen_llvm {

ir::Storage hello_world_ir(str::StringInterner* interner) {
  ir::StorageBuilder builder;

  const str::StringPoolId main_str = interner->intern("main");
  const str::StringPoolId puts_str = interner->intern("puts");
  const str::StringPoolId hello_str = interner->intern("Hello, World!");

  const ir::ImmutableIdx imm_1 = builder.immutable(
      {.type = ir::primitive_idx(ir::TypeTag::I32), .data = {.i32_value = 1}});

  const ir::ImmutableIdx imm_hello =
      builder.immutable({.type = ir::primitive_idx(ir::TypeTag::Str),
                         .data = {.str_id_value = hello_str}});

  const ir::TypeIdx puts_param_type_id = builder.primitive(ir::TypeTag::Ptr);
  const ir::ExternalFunctionIdx func_puts = builder.external_function({
      .meta = {.return_type = ir::primitive_idx(ir::TypeTag::I32),
               .param_types = {puts_param_type_id, 1},
               .name = puts_str},
      .calling_conv = ir::CallingConvention::C,
  });

  // reg 0 := 1 - 1 = 0
  ir::OperandSeq sub_args;
  sub_args.push(builder.operand(
      ir::Operand::from_immutable(imm_1, ir::primitive_idx(ir::TypeTag::I32))));
  sub_args.push(builder.operand(
      ir::Operand::from_immutable(imm_1, ir::primitive_idx(ir::TypeTag::I32))));
  const ir::InstructionIdx inst_sub = builder.instr({
      .op = ir::Opcode::IntSub,
      .flags = {},
      .dst = ir::RegisterIdx(0),
      .operands = sub_args.finish(),
  });
  builder.reg(
      {.type = ir::primitive_idx(ir::TypeTag::I32), .def_idx = inst_sub});

  const ir::RegisterIdx ret_register(1);
  ir::OperandSeq call_args;
  call_args.push(builder.operand(ir::Operand::from_external_function(
      func_puts, ir::primitive_idx(ir::TypeTag::Function))));
  call_args.push(builder.operand(ir::Operand::from_immutable(
      imm_hello, ir::primitive_idx(ir::TypeTag::Str))));
  const ir::InstructionIdx inst_call = builder.instr({
      .op = ir::Opcode::Call,
      .flags = {},
      .dst = ret_register,
      .operands = call_args.finish(),
  });
  builder.reg(
      {.type = ir::primitive_idx(ir::TypeTag::I32), .def_idx = inst_call});

  const ir::OperandIdx reg_op = builder.operand(ir::Operand::from_register(
      ret_register, ir::primitive_idx(ir::TypeTag::I32)));
  const ir::InstructionIdx inst_ret = builder.instr({
      .op = ir::Opcode::Ret,
      .flags = {},
      .dst = ir::RegisterIdx(base::kInvalidIdx),
      .operands = {reg_op, 1},
  });

  ir::InstrSeq instrs;
  instrs.push(inst_sub);
  instrs.push(inst_call);
  instrs.push(inst_ret);
  const ir::BlockIdx block = builder.block({
      .instrs = instrs.finish(),
      .block_params = {},
  });

  builder.function({
      .meta = {.return_type = ir::primitive_idx(ir::TypeTag::I32),
               .param_types = {},
               .name = main_str},
      .blocks = {block, 1},
  });

  return std::move(builder).build();
}

TEST_CASE("Emit Hello World") {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("llvm_ir_emitter_test", context);

  str::StringInterner interner(mem::page_size());
  ir::Storage storage = hello_world_ir(&interner);
  LlvmIrEmitter emitter(module.get(), std::move(storage), &interner);

  std::move(emitter).emit();

  CHECK(!llvm::verifyModule(*module));

  std::string ir_str;
  llvm::raw_string_ostream os(ir_str);
  module->print(os, nullptr);

  tests::logger.debug("LLVM IR dump:\n{}", ir_str);
}

TEST_CASE("Emit struct and array calls") {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("struct_array_test", context);

  str::StringInterner interner(mem::page_size());
  ir::StorageBuilder builder;

  const ir::TypeIdx i32 = builder.primitive(ir::TypeTag::I32);
  ir::TypeSeq fields;
  fields.push(builder.ref_type(i32));
  fields.push(builder.ref_type(i32));
  const ir::TypeIdx pair =
      builder.struct_type(interner.intern("Pair"), fields.finish());
  const ir::TypeIdx arr4 = builder.array_type(i32, 4);

  const ir::ExternalFunctionIdx func_mkpair = builder.external_function({
      .meta = {.return_type = pair,
               .param_types = {},
               .name = interner.intern("makepair")},
      .calling_conv = ir::CallingConvention::C,
  });
  const ir::ExternalFunctionIdx func_mkarr = builder.external_function({
      .meta = {.return_type = arr4,
               .param_types = {},
               .name = interner.intern("mkarr")},
      .calling_conv = ir::CallingConvention::C,
  });

  // @testpair() -> {i32, i32} { %r = call @makepair(); ret %r }
  const ir::OperandIdx mkpair_op =
      builder.operand(ir::Operand::from_external_function(func_mkpair, pair));
  const ir::InstructionIdx inst_call = builder.instr({
      .op = ir::Opcode::Call,
      .flags = {},
      .dst = ir::RegisterIdx(0),
      .operands = {mkpair_op, 1},
  });
  builder.reg({.type = pair, .def_idx = inst_call});
  const ir::OperandIdx ret_op =
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(0), pair));
  const ir::InstructionIdx inst_ret = builder.instr({
      .op = ir::Opcode::Ret,
      .flags = {},
      .dst = ir::RegisterIdx(base::kInvalidIdx),
      .operands = {ret_op, 1},
  });
  ir::InstrSeq instrs;
  instrs.push(inst_call);
  instrs.push(inst_ret);
  const ir::BlockIdx block = builder.block({
      .instrs = instrs.finish(),
      .block_params = {},
  });
  builder.function({
      .meta = {.return_type = pair,
               .param_types = {},
               .name = interner.intern("testpair")},
      .blocks = {block, 1},
  });

  // @testarr() -> [4 x i32] { %r = call @mkarr(); ret %r }
  const ir::OperandIdx mkarr_op =
      builder.operand(ir::Operand::from_external_function(func_mkarr, arr4));
  const ir::InstructionIdx inst_call2 = builder.instr({
      .op = ir::Opcode::Call,
      .flags = {},
      .dst = ir::RegisterIdx(1),
      .operands = {mkarr_op, 1},
  });
  builder.reg({.type = arr4, .def_idx = inst_call2});
  const ir::OperandIdx ret_op2 =
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(1), arr4));
  const ir::InstructionIdx inst_ret2 = builder.instr({
      .op = ir::Opcode::Ret,
      .flags = {},
      .dst = ir::RegisterIdx(base::kInvalidIdx),
      .operands = {ret_op2, 1},
  });
  ir::InstrSeq instrs2;
  instrs2.push(inst_call2);
  instrs2.push(inst_ret2);
  const ir::BlockIdx block2 = builder.block({
      .instrs = instrs2.finish(),
      .block_params = {},
  });
  builder.function({
      .meta = {.return_type = arr4,
               .param_types = {},
               .name = interner.intern("testarr")},
      .blocks = {block2, 1},
  });

  ir::Storage storage = std::move(builder).build();
  LlvmIrEmitter emitter(module.get(), std::move(storage), &interner);

  std::move(emitter).emit();

  CHECK(!llvm::verifyModule(*module));

  std::string ir_str;
  llvm::raw_string_ostream os(ir_str);
  module->print(os, nullptr);

  CHECK(ir_str.find("{ i32, i32 }") != std::string::npos);
  CHECK(ir_str.find("[4 x i32]") != std::string::npos);

  tests::logger.debug("LLVM IR dump:\n{}", ir_str);
}

}  // namespace codegen_llvm
