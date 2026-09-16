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
#include "ir/operand.h"
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

  const ir::ImmutableIdx imm_1 =
      builder.immutable({.type = ir::Type::I32, .data = {.i32_value = 1}});

  const ir::ImmutableIdx imm_hello = builder.immutable(
      {.type = ir::Type::Str, .data = {.str_id_value = hello_str}});

  const ir::TypeIdx puts_param_type_id = builder.type(ir::Type::Ptr);
  const ir::ExternalFunctionIdx func_puts = builder.external_function({
      .meta = {.return_type = ir::Type::I32,
               .param_types = {puts_param_type_id, 1},
               .name = puts_str},
      .calling_conv = ir::CallingConvention::C,
  });

  // reg 0 := 1 - 1 = 0
  const ir::OperandIdx lhs =
      builder.operand(ir::Operand::from_immutable(imm_1, ir::Type::I32));
  const ir::OperandIdx rhs =
      builder.operand(ir::Operand::from_immutable(imm_1, ir::Type::I32));
  const ir::InstructionIdx inst_sub = builder.instr({
      .op = ir::Opcode::IntSub,
      .flags = {},
      .dst = ir::RegisterIdx(0),
      .operands = ir::OperandIdxRange::from_to(lhs, rhs),
  });
  builder.reg({.type = ir::Type::I32, .def_idx = inst_sub});

  const ir::OperandIdx puts_op = builder.operand(
      ir::Operand::from_external_function(func_puts, ir::Type::Function));
  const ir::OperandIdx arg_str_op =
      builder.operand(ir::Operand::from_immutable(imm_hello, ir::Type::Str));

  const ir::RegisterIdx ret_register(0);
  const ir::OperandIdx reg_op =
      builder.operand(ir::Operand::from_register(ret_register, ir::Type::I32));
  /* const ir::InstructionId inst_call = */ builder.instr({
      .op = ir::Opcode::Call,
      .flags = {},
      .dst = ret_register,
      .operands = ir::OperandIdxRange::from_to(puts_op, arg_str_op),
  });

  const ir::InstructionIdx inst_ret = builder.instr({
      .op = ir::Opcode::Ret,
      .flags = {},
      .dst = ir::RegisterIdx(base::kInvalidIdx),
      .operands = {reg_op, 1},
  });

  const ir::BlockIdx block = builder.block({
      .instrs = ir::InstructionIdxRange::from_to(inst_sub, inst_ret),
      .block_params = {},
  });

  builder.function({
      .meta = {.return_type = ir::Type::I32,
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

}  // namespace codegen_llvm
