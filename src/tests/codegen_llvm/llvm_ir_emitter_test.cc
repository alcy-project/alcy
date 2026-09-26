// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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
#include "ir/function.h"
#include "ir/instruction_flags.h"
#include "ir/opcode.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace codegen_llvm {

ir::VerifiedStorage hello_world_ir(str::StringInterner* interner) {
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
               .name = puts_str,
               .path = str::kEmptyStringId,
               .kind = ir::SymbolKind::Foreign,
               .generics = ir::TypeIdxRange{}},
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
      .measure = ir::TypeIdx::invalid(),
      .operands = sub_args.finish(),
  });
  builder.reg(
      {.type = ir::primitive_idx(ir::TypeTag::I32), .def_idx = inst_sub});

  // Fat strings cross as structs; extract the byte pointer first.
  const ir::ImmutableIdx imm_zero = builder.immutable(
      {.type = ir::primitive_idx(ir::TypeTag::I32), .data = {.i32_value = 0}});
  ir::OperandSeq extract_args;
  extract_args.push(builder.operand(ir::Operand::from_immutable(
      imm_hello, ir::primitive_idx(ir::TypeTag::Str))));
  extract_args.push(builder.operand(ir::Operand::from_immutable(
      imm_zero, ir::primitive_idx(ir::TypeTag::I32))));
  const ir::InstructionIdx inst_extract = builder.instr({
      .op = ir::Opcode::ExtractValue,
      .flags = {},
      .dst = ir::RegisterIdx(1),
      .measure = ir::TypeIdx::invalid(),
      .operands = extract_args.finish(),
  });
  builder.reg(
      {.type = ir::primitive_idx(ir::TypeTag::Ptr), .def_idx = inst_extract});

  const ir::RegisterIdx ret_register(2);
  ir::OperandSeq call_args;
  call_args.push(builder.operand(ir::Operand::from_external_function(
      func_puts, ir::primitive_idx(ir::TypeTag::Function))));
  call_args.push(builder.operand(ir::Operand::from_register(
      ir::RegisterIdx(1), ir::primitive_idx(ir::TypeTag::Ptr))));
  const ir::InstructionIdx inst_call = builder.instr({
      .op = ir::Opcode::Call,
      .flags = {},
      .dst = ret_register,
      .measure = ir::TypeIdx::invalid(),
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
      .measure = ir::TypeIdx::invalid(),
      .operands = {reg_op, 1},
  });

  ir::InstrSeq instrs;
  instrs.push(inst_sub);
  instrs.push(inst_extract);
  instrs.push(inst_call);
  instrs.push(inst_ret);
  const ir::BlockIdx block = builder.block({
      .instrs = instrs.finish(),
      .block_params = {},
  });

  builder.function({
      .meta = {.return_type = ir::primitive_idx(ir::TypeTag::I32),
               .param_types = {},
               .name = main_str,
               .path = str::kEmptyStringId,
               .kind = ir::SymbolKind::Foreign,
               .generics = ir::TypeIdxRange{}},
      .blocks = {block, 1},
  });

  return std::move(builder).build().unwrap();
}

TEST_CASE("Emit Hello World") {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("llvm_ir_emitter_test", context);

  str::StringInterner interner(mem::page_size());
  ir::VerifiedStorage storage = hello_world_ir(&interner);
  LlvmIrEmitter emitter(module.get(), std::move(storage), &interner,
                        ir::PointerWidth::W64);

  std::move(emitter).emit();

  CHECK(!llvm::verifyModule(*module));

  std::string ir_str;
  llvm::raw_string_ostream os(ir_str);
  module->print(os, nullptr);
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
  const ir::TypeIdx pair = builder.struct_type(
      interner.intern("Pair"), fields.finish(), ir::TypeIdxRange{});
  const ir::TypeIdx arr4 = builder.array_type(i32, 4);

  const ir::ExternalFunctionIdx func_mkpair = builder.external_function({
      .meta = {.return_type = pair,
               .param_types = {},
               .name = interner.intern("makepair"),
               .path = str::kEmptyStringId,
               .kind = ir::SymbolKind::Foreign,
               .generics = ir::TypeIdxRange{}},
      .calling_conv = ir::CallingConvention::C,
  });
  const ir::ExternalFunctionIdx func_mkarr = builder.external_function({
      .meta = {.return_type = arr4,
               .param_types = {},
               .name = interner.intern("mkarr"),
               .path = str::kEmptyStringId,
               .kind = ir::SymbolKind::Foreign,
               .generics = ir::TypeIdxRange{}},
      .calling_conv = ir::CallingConvention::C,
  });

  // @testpair() -> {i32, i32} { %r = call @makepair(); ret %r }
  const ir::OperandIdx mkpair_op =
      builder.operand(ir::Operand::from_external_function(func_mkpair, pair));
  const ir::InstructionIdx inst_call = builder.instr({
      .op = ir::Opcode::Call,
      .flags = {},
      .dst = ir::RegisterIdx(0),
      .measure = ir::TypeIdx::invalid(),
      .operands = {mkpair_op, 1},
  });
  builder.reg({.type = pair, .def_idx = inst_call});
  const ir::OperandIdx ret_op =
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(0), pair));
  const ir::InstructionIdx inst_ret = builder.instr({
      .op = ir::Opcode::Ret,
      .flags = {},
      .dst = ir::RegisterIdx(base::kInvalidIdx),
      .measure = ir::TypeIdx::invalid(),
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
               .name = interner.intern("testpair"),
               .path = str::kEmptyStringId,
               .kind = ir::SymbolKind::Foreign,
               .generics = ir::TypeIdxRange{}},
      .blocks = {block, 1},
  });

  // @testarr() -> [4 x i32] { %r = call @mkarr(); ret %r }
  const ir::OperandIdx mkarr_op =
      builder.operand(ir::Operand::from_external_function(func_mkarr, arr4));
  const ir::InstructionIdx inst_call2 = builder.instr({
      .op = ir::Opcode::Call,
      .flags = {},
      .dst = ir::RegisterIdx(1),
      .measure = ir::TypeIdx::invalid(),
      .operands = {mkarr_op, 1},
  });
  builder.reg({.type = arr4, .def_idx = inst_call2});
  const ir::OperandIdx ret_op2 =
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(1), arr4));
  const ir::InstructionIdx inst_ret2 = builder.instr({
      .op = ir::Opcode::Ret,
      .flags = {},
      .dst = ir::RegisterIdx(base::kInvalidIdx),
      .measure = ir::TypeIdx::invalid(),
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
               .name = interner.intern("testarr"),
               .path = str::kEmptyStringId,
               .kind = ir::SymbolKind::Foreign,
               .generics = ir::TypeIdxRange{}},
      .blocks = {block2, 1},
  });

  ir::VerifiedStorage storage = std::move(builder).build().unwrap();
  LlvmIrEmitter emitter(module.get(), std::move(storage), &interner,
                        ir::PointerWidth::W64);

  std::move(emitter).emit();

  CHECK(!llvm::verifyModule(*module));

  std::string ir_str;
  llvm::raw_string_ostream os(ir_str);
  module->print(os, nullptr);

  CHECK(ir_str.find("{ i32, i32 }") != std::string::npos);
  CHECK(ir_str.find("[4 x i32]") != std::string::npos);
}

TEST_CASE("Emit compute instructions") {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("compute_test", context);

  str::StringInterner interner(mem::page_size());
  ir::StorageBuilder builder;

  const ir::TypeIdx i32 = builder.primitive(ir::TypeTag::I32);
  const ir::TypeIdx i1 = builder.primitive(ir::TypeTag::I1);

  // @arith(i32 %a, i32 %b) -> i32, entry block takes both as parameters.
  ir::TypeSeq params;
  params.push(builder.ref_type(i32));
  params.push(builder.ref_type(i32));

  builder.reg({.type = i32, .def_idx = ir::InstructionIdx(base::kInvalidIdx)});
  builder.reg({.type = i32, .def_idx = ir::InstructionIdx(base::kInvalidIdx)});
  ir::BlockParamSeq block_params;
  block_params.push(
      builder.block_param({.type = i32, .reg = ir::RegisterIdx(0)}));
  block_params.push(
      builder.block_param({.type = i32, .reg = ir::RegisterIdx(1)}));

  ir::InstrSeq instrs;
  // Helper appending a binary instruction; dst registers are numbered
  // sequentially (r2 and up) matching reg() calls below.
  auto binary = [&](ir::Opcode op, ir::RegisterIdx lhs, ir::RegisterIdx rhs,
                    ir::TypeIdx operand_ty, ir::RegisterIdx dst) {
    ir::OperandSeq args;
    args.push(builder.operand(ir::Operand::from_register(lhs, operand_ty)));
    args.push(builder.operand(ir::Operand::from_register(rhs, operand_ty)));
    const ir::InstructionIdx inst =
        builder.instr({.op = op,
                       .flags = {},
                       .dst = dst,
                       .measure = ir::TypeIdx::invalid(),
                       .operands = args.finish()});
    instrs.push(inst);
    return inst;
  };

  // div = a / b (signed); rem = a % b (unsigned)
  const ir::InstructionIdx inst_div =
      binary(ir::Opcode::IntDiv, ir::RegisterIdx(0), ir::RegisterIdx(1), i32,
             ir::RegisterIdx(2));
  builder.reg({.type = i32, .def_idx = inst_div});
  const ir::InstructionIdx inst_rem =
      binary(ir::Opcode::UintRem, ir::RegisterIdx(0), ir::RegisterIdx(1), i32,
             ir::RegisterIdx(3));
  builder.reg({.type = i32, .def_idx = inst_rem});
  // cmp = div < rem (signed); ext = (i32)cmp; sel = cmp ? div : rem
  const ir::InstructionIdx inst_cmp =
      binary(ir::Opcode::Lt, ir::RegisterIdx(2), ir::RegisterIdx(3), i32,
             ir::RegisterIdx(4));
  builder.reg({.type = i1, .def_idx = inst_cmp});
  const ir::OperandIdx cast_arg =
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(4), i1));
  const ir::InstructionIdx inst_cast =
      builder.instr({.op = ir::Opcode::TypeCast,
                     .flags = {},
                     .dst = ir::RegisterIdx(5),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {cast_arg, 1}});
  instrs.push(inst_cast);
  builder.reg({.type = i32, .def_idx = inst_cast});
  ir::OperandSeq sel_args;
  sel_args.push(
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(4), i1)));
  sel_args.push(
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(2), i32)));
  sel_args.push(
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(3), i32)));
  const ir::InstructionIdx inst_sel =
      builder.instr({.op = ir::Opcode::Select,
                     .flags = {},
                     .dst = ir::RegisterIdx(6),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = sel_args.finish()});
  instrs.push(inst_sel);
  builder.reg({.type = i32, .def_idx = inst_sel});
  // add = ext + sel; not = ~add; rev = bitreverse(not); ret = move(rev)
  const ir::InstructionIdx inst_add =
      binary(ir::Opcode::IntAdd, ir::RegisterIdx(5), ir::RegisterIdx(6), i32,
             ir::RegisterIdx(7));
  builder.reg({.type = i32, .def_idx = inst_add});
  const ir::OperandIdx not_arg =
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(7), i32));
  const ir::InstructionIdx inst_not =
      builder.instr({.op = ir::Opcode::Not,
                     .flags = {},
                     .dst = ir::RegisterIdx(8),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {not_arg, 1}});
  instrs.push(inst_not);
  builder.reg({.type = i32, .def_idx = inst_not});
  const ir::OperandIdx rev_arg =
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(8), i32));
  const ir::InstructionIdx inst_rev =
      builder.instr({.op = ir::Opcode::BitReverse,
                     .flags = {},
                     .dst = ir::RegisterIdx(9),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {rev_arg, 1}});
  instrs.push(inst_rev);
  builder.reg({.type = i32, .def_idx = inst_rev});
  const ir::OperandIdx mov_arg =
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(9), i32));
  const ir::InstructionIdx inst_mov =
      builder.instr({.op = ir::Opcode::Move,
                     .flags = {},
                     .dst = ir::RegisterIdx(10),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {mov_arg, 1}});
  instrs.push(inst_mov);
  builder.reg({.type = i32, .def_idx = inst_mov});
  const ir::OperandIdx drop_arg =
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(10), i32));
  const ir::InstructionIdx inst_drop =
      builder.instr({.op = ir::Opcode::Drop,
                     .flags = {},
                     .dst = ir::RegisterIdx(base::kInvalidIdx),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {drop_arg, 1}});
  instrs.push(inst_drop);
  const ir::OperandIdx ret_arg =
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(10), i32));
  const ir::InstructionIdx inst_ret =
      builder.instr({.op = ir::Opcode::Ret,
                     .flags = {},
                     .dst = ir::RegisterIdx(base::kInvalidIdx),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {ret_arg, 1}});
  instrs.push(inst_ret);

  const ir::BlockIdx block = builder.block({
      .instrs = instrs.finish(),
      .block_params = block_params.finish(),
  });
  builder.function({
      .meta = {.return_type = i32,
               .param_types = params.finish(),
               .name = interner.intern("arith"),
               .path = str::kEmptyStringId,
               .kind = ir::SymbolKind::Foreign,
               .generics = ir::TypeIdxRange{}},
      .blocks = {block, 1},
  });

  ir::VerifiedStorage storage = std::move(builder).build().unwrap();
  LlvmIrEmitter emitter(module.get(), std::move(storage), &interner,
                        ir::PointerWidth::W64);

  std::move(emitter).emit();

  CHECK(!llvm::verifyModule(*module));

  std::string ir_str;
  llvm::raw_string_ostream os(ir_str);
  module->print(os, nullptr);

  CHECK(ir_str.find("sdiv") != std::string::npos);
  CHECK(ir_str.find("urem") != std::string::npos);
  CHECK(ir_str.find("icmp slt") != std::string::npos);
  CHECK(ir_str.find("zext") != std::string::npos);
  CHECK(ir_str.find("select") != std::string::npos);
  CHECK(ir_str.find("bitreverse") != std::string::npos);
}

TEST_CASE("Emit control flow") {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("control_test", context);

  str::StringInterner interner(mem::page_size());
  ir::StorageBuilder builder;

  const ir::TypeIdx i32 = builder.primitive(ir::TypeTag::I32);
  const ir::TypeIdx i1 = builder.primitive(ir::TypeTag::I1);
  const ir::ImmutableIdx one =
      builder.immutable({.type = i32, .data = {.i32_value = 1}});
  const ir::ImmutableIdx zero =
      builder.immutable({.type = i32, .data = {.i32_value = 0}});
  const ir::ImmutableIdx two =
      builder.immutable({.type = i32, .data = {.i32_value = 2}});
  const ir::ImmutableIdx ten =
      builder.immutable({.type = i32, .data = {.i32_value = 10}});
  const ir::ImmutableIdx twenty =
      builder.immutable({.type = i32, .data = {.i32_value = 20}});

  // @condbr(i1 %c) -> i32 with entry/then/else blocks.
  ir::TypeSeq cond_params;
  cond_params.push(builder.ref_type(i1));
  builder.reg({.type = i1, .def_idx = ir::InstructionIdx(base::kInvalidIdx)});
  const ir::BlockParamIdx cond_param =
      builder.block_param({.type = i1, .reg = ir::RegisterIdx(0)});

  auto ret_block = [&](ir::ImmutableIdx value) {
    ir::OperandSeq args;
    args.push(builder.operand(ir::Operand::from_immutable(value, i32)));
    const ir::InstructionIdx inst =
        builder.instr({.op = ir::Opcode::Ret,
                       .flags = {},
                       .dst = ir::RegisterIdx(base::kInvalidIdx),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = args.finish()});
    ir::InstrSeq instrs;
    instrs.push(inst);
    return builder.block({.instrs = instrs.finish(), .block_params = {}});
  };

  // @condbr(i1 %c) -> i32 with entry/then/else blocks. The entry block is
  // declared empty first (LLVM requires it first in vector order) and
  // backpatched once the targets exist.
  const ir::BlockIdx cond_entry = builder.block({{}, {}});
  const ir::BlockIdx then_block = ret_block(one);
  const ir::BlockIdx else_block = ret_block(zero);

  ir::OperandSeq cond_args;
  cond_args.push(
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(0), i1)));
  cond_args.push(builder.operand(ir::Operand::from_block(then_block, i32)));
  cond_args.push(builder.operand(ir::Operand::from_block(else_block, i32)));
  const ir::InstructionIdx inst_condbr =
      builder.instr({.op = ir::Opcode::CondBr,
                     .flags = {},
                     .dst = ir::RegisterIdx(base::kInvalidIdx),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = cond_args.finish()});
  ir::InstrSeq entry_instrs;
  entry_instrs.push(inst_condbr);
  builder.set_block_instrs(cond_entry, entry_instrs.finish());
  ir::BlockParamSeq entry_params;
  entry_params.push(cond_param);
  builder.set_block_params(cond_entry, entry_params.finish());
  ir::BlockSeq cond_blocks;
  cond_blocks.push(cond_entry);
  cond_blocks.push(then_block);
  cond_blocks.push(else_block);
  builder.function({
      .meta = {.return_type = i32,
               .param_types = cond_params.finish(),
               .name = interner.intern("condbr"),
               .path = str::kEmptyStringId,
               .kind = ir::SymbolKind::Foreign,
               .generics = ir::TypeIdxRange{}},
      .blocks = cond_blocks.finish(),
  });

  // @sw(i32 %v) -> i32 with entry/default/case blocks.
  builder.reg({.type = i32, .def_idx = ir::InstructionIdx(base::kInvalidIdx)});
  const ir::BlockIdx sw_entry = builder.block({{}, {}});
  const ir::BlockIdx sw_default = ret_block(zero);
  const ir::BlockIdx sw_case1 = ret_block(ten);
  const ir::BlockIdx sw_case2 = ret_block(twenty);
  ir::OperandSeq sw_args;
  sw_args.push(
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(1), i32)));
  sw_args.push(builder.operand(ir::Operand::from_block(sw_default, i32)));
  sw_args.push(builder.operand(ir::Operand::from_immutable(one, i32)));
  sw_args.push(builder.operand(ir::Operand::from_block(sw_case1, i32)));
  sw_args.push(builder.operand(ir::Operand::from_immutable(two, i32)));
  sw_args.push(builder.operand(ir::Operand::from_block(sw_case2, i32)));
  const ir::InstructionIdx inst_sw =
      builder.instr({.op = ir::Opcode::Switch,
                     .flags = {},
                     .dst = ir::RegisterIdx(base::kInvalidIdx),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = sw_args.finish()});
  ir::InstrSeq sw_instrs;
  sw_instrs.push(inst_sw);
  builder.set_block_instrs(sw_entry, sw_instrs.finish());
  ir::BlockParamSeq sw_params;
  sw_params.push(builder.block_param({.type = i32, .reg = ir::RegisterIdx(1)}));
  builder.set_block_params(sw_entry, sw_params.finish());
  ir::TypeSeq sw_fn_params;
  sw_fn_params.push(builder.ref_type(i32));
  ir::BlockSeq sw_blocks;
  sw_blocks.push(sw_entry);
  sw_blocks.push(sw_default);
  sw_blocks.push(sw_case1);
  sw_blocks.push(sw_case2);
  builder.function({
      .meta = {.return_type = i32,
               .param_types = sw_fn_params.finish(),
               .name = interner.intern("sw"),
               .path = str::kEmptyStringId,
               .kind = ir::SymbolKind::Foreign,
               .generics = ir::TypeIdxRange{}},
      .blocks = sw_blocks.finish(),
  });

  ir::VerifiedStorage storage = std::move(builder).build().unwrap();
  LlvmIrEmitter emitter(module.get(), std::move(storage), &interner,
                        ir::PointerWidth::W64);

  std::move(emitter).emit();

  CHECK(!llvm::verifyModule(*module));

  std::string ir_str;
  llvm::raw_string_ostream os(ir_str);
  module->print(os, nullptr);

  CHECK(ir_str.find("br i1") != std::string::npos);
  CHECK(ir_str.find("switch i32") != std::string::npos);
}

TEST_CASE("Emit memory instructions") {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("memory_test", context);

  str::StringInterner interner(mem::page_size());
  ir::StorageBuilder builder;

  const ir::TypeIdx i32 = builder.primitive(ir::TypeTag::I32);
  ir::TypeSeq pair_fields;
  pair_fields.push(builder.ref_type(i32));
  pair_fields.push(builder.ref_type(i32));
  const ir::TypeIdx pair = builder.struct_type(
      interner.intern("Pair"), pair_fields.finish(), ir::TypeIdxRange{});

  auto imm_op = [&](ir::ImmutableIdx imm, ir::TypeIdx ty) {
    return builder.operand(ir::Operand::from_immutable(imm, ty));
  };
  auto reg_op = [&](ir::RegisterIdx reg, ir::TypeIdx ty) {
    return builder.operand(ir::Operand::from_register(reg, ty));
  };

  const ir::ImmutableIdx one =
      builder.immutable({.type = i32, .data = {.i32_value = 1}});
  const ir::ImmutableIdx val42 =
      builder.immutable({.type = i32, .data = {.i32_value = 42}});
  const ir::ImmutableIdx val7 =
      builder.immutable({.type = i32, .data = {.i32_value = 7}});
  const ir::ImmutableIdx zero =
      builder.immutable({.type = i32, .data = {.i32_value = 0}});

  ir::InstrSeq instrs;
  auto unary = [&](ir::Opcode op, ir::OperandIdx arg, ir::RegisterIdx dst,
                   ir::TypeIdx dst_ty) {
    ir::OperandSeq args;
    args.push(arg);
    const ir::InstructionIdx inst =
        builder.instr({.op = op,
                       .flags = {},
                       .dst = dst,
                       .measure = ir::TypeIdx::invalid(),
                       .operands = args.finish()});
    instrs.push(inst);
    builder.reg({.type = dst_ty, .def_idx = inst});
    return inst;
  };

  // p = alloca i32; store 42 -> [p]; v = load [p]
  const ir::InstructionIdx inst_alloc =
      builder.instr({.op = ir::Opcode::Alloca,
                     .flags = {},
                     .dst = ir::RegisterIdx(0),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {imm_op(one, i32), 1}});
  instrs.push(inst_alloc);
  builder.reg({.type = i32, .def_idx = inst_alloc});
  ir::OperandSeq store_args;
  store_args.push(imm_op(val42, i32));
  store_args.push(
      reg_op(ir::RegisterIdx(0), ir::primitive_idx(ir::TypeTag::Ptr)));
  instrs.push(builder.instr({.op = ir::Opcode::Store,
                             .flags = {},
                             .dst = ir::RegisterIdx(base::kInvalidIdx),
                             .measure = ir::TypeIdx::invalid(),
                             .operands = store_args.finish()}));
  unary(ir::Opcode::Load,
        reg_op(ir::RegisterIdx(0), ir::primitive_idx(ir::TypeTag::Ptr)),
        ir::RegisterIdx(1), i32);

  // atomic store/load/rmw/cmpxchg + fence
  ir::OperandSeq astore_args;
  astore_args.push(imm_op(val7, i32));
  astore_args.push(
      reg_op(ir::RegisterIdx(0), ir::primitive_idx(ir::TypeTag::Ptr)));
  instrs.push(builder.instr({.op = ir::Opcode::AtomicStore,
                             .flags = {},
                             .dst = ir::RegisterIdx(base::kInvalidIdx),
                             .measure = ir::TypeIdx::invalid(),
                             .operands = astore_args.finish()}));
  unary(ir::Opcode::AtomicLoad,
        reg_op(ir::RegisterIdx(0), ir::primitive_idx(ir::TypeTag::Ptr)),
        ir::RegisterIdx(2), i32);
  ir::OperandSeq rmw_args;
  rmw_args.push(
      reg_op(ir::RegisterIdx(0), ir::primitive_idx(ir::TypeTag::Ptr)));
  rmw_args.push(reg_op(ir::RegisterIdx(2), i32));
  const ir::InstructionIdx inst_rmw =
      builder.instr({.op = ir::Opcode::AtomicRmw,
                     .flags = {.rmw_op = ir::AtomicRmwOp::Add},
                     .dst = ir::RegisterIdx(3),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = rmw_args.finish()});
  instrs.push(inst_rmw);
  builder.reg({.type = i32, .def_idx = inst_rmw});

  // r4 = cmpxchg [r0] cmp r2 -> r3 : {i32, i1}
  ir::TypeSeq cmpxchg_fields;
  cmpxchg_fields.push(builder.ref_type(i32));
  cmpxchg_fields.push(builder.ref_type(builder.primitive(ir::TypeTag::I1)));
  const ir::TypeIdx pair_i1 =
      builder.struct_type(interner.intern("CmpXchgRes"),
                          cmpxchg_fields.finish(), ir::TypeIdxRange{});
  ir::OperandSeq cmpxchg_args;
  cmpxchg_args.push(
      reg_op(ir::RegisterIdx(0), ir::primitive_idx(ir::TypeTag::Ptr)));
  cmpxchg_args.push(reg_op(ir::RegisterIdx(2), i32));
  cmpxchg_args.push(reg_op(ir::RegisterIdx(3), i32));
  const ir::InstructionIdx inst_cmpxchg =
      builder.instr({.op = ir::Opcode::AtomicCompareExchange,
                     .flags = {},
                     .dst = ir::RegisterIdx(4),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = cmpxchg_args.finish()});
  instrs.push(inst_cmpxchg);
  builder.reg({.type = pair_i1, .def_idx = inst_cmpxchg});
  instrs.push(builder.instr({.op = ir::Opcode::Fence,
                             .flags = {},
                             .dst = ir::RegisterIdx(base::kInvalidIdx),
                             .measure = ir::TypeIdx::invalid(),
                             .operands = {}}));

  // ps = alloca Pair; p1 = gep [ps, 0, 1]; store r1 -> [p1]; f1 = load [p1]
  const ir::InstructionIdx inst_alloc_struct =
      builder.instr({.op = ir::Opcode::Alloca,
                     .flags = {},
                     .dst = ir::RegisterIdx(5),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {imm_op(one, i32), 1}});
  instrs.push(inst_alloc_struct);
  builder.reg({.type = pair, .def_idx = inst_alloc_struct});
  ir::OperandSeq gep_args;
  gep_args.push(
      reg_op(ir::RegisterIdx(5), ir::primitive_idx(ir::TypeTag::Ptr)));
  gep_args.push(imm_op(zero, i32));
  gep_args.push(imm_op(one, i32));
  const ir::InstructionIdx inst_gep =
      builder.instr({.op = ir::Opcode::GetElementPtr,
                     .flags = {},
                     .dst = ir::RegisterIdx(6),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = gep_args.finish()});
  instrs.push(inst_gep);
  builder.reg(
      {.type = ir::primitive_idx(ir::TypeTag::Ptr), .def_idx = inst_gep});
  ir::OperandSeq field_store_args;
  field_store_args.push(reg_op(ir::RegisterIdx(1), i32));
  field_store_args.push(
      reg_op(ir::RegisterIdx(6), ir::primitive_idx(ir::TypeTag::Ptr)));
  instrs.push(builder.instr({.op = ir::Opcode::Store,
                             .flags = {},
                             .dst = ir::RegisterIdx(base::kInvalidIdx),
                             .measure = ir::TypeIdx::invalid(),
                             .operands = field_store_args.finish()}));
  unary(ir::Opcode::Load,
        reg_op(ir::RegisterIdx(6), ir::primitive_idx(ir::TypeTag::Ptr)),
        ir::RegisterIdx(7), i32);

  // Insert/extract roundtrip on the cmpxchg result struct.
  ir::OperandSeq ext_args;
  ext_args.push(reg_op(ir::RegisterIdx(4), pair_i1));
  ext_args.push(imm_op(zero, i32));
  const ir::InstructionIdx inst_ext =
      builder.instr({.op = ir::Opcode::ExtractValue,
                     .flags = {},
                     .dst = ir::RegisterIdx(8),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = ext_args.finish()});
  instrs.push(inst_ext);
  builder.reg({.type = i32, .def_idx = inst_ext});
  ir::OperandSeq ins_args;
  ins_args.push(reg_op(ir::RegisterIdx(4), pair_i1));
  ins_args.push(reg_op(ir::RegisterIdx(8), i32));
  ins_args.push(imm_op(zero, i32));
  const ir::InstructionIdx inst_ins =
      builder.instr({.op = ir::Opcode::InsertValue,
                     .flags = {},
                     .dst = ir::RegisterIdx(9),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = ins_args.finish()});
  instrs.push(inst_ins);
  builder.reg({.type = pair_i1, .def_idx = inst_ins});
  ir::OperandSeq ext_args2;
  ext_args2.push(reg_op(ir::RegisterIdx(9), pair_i1));
  ext_args2.push(imm_op(zero, i32));
  const ir::InstructionIdx inst_ext2 =
      builder.instr({.op = ir::Opcode::ExtractValue,
                     .flags = {},
                     .dst = ir::RegisterIdx(10),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = ext_args2.finish()});
  instrs.push(inst_ext2);
  builder.reg({.type = i32, .def_idx = inst_ext2});

  const ir::InstructionIdx inst_ret =
      builder.instr({.op = ir::Opcode::Ret,
                     .flags = {},
                     .dst = ir::RegisterIdx(base::kInvalidIdx),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {reg_op(ir::RegisterIdx(10), i32), 1}});
  instrs.push(inst_ret);
  const ir::BlockIdx block =
      builder.block({.instrs = instrs.finish(), .block_params = {}});
  builder.function({
      .meta = {.return_type = i32,
               .param_types = {},
               .name = interner.intern("memtest"),
               .path = str::kEmptyStringId,
               .kind = ir::SymbolKind::Foreign,
               .generics = ir::TypeIdxRange{}},
      .blocks = {block, 1},
  });

  ir::VerifiedStorage storage = std::move(builder).build().unwrap();
  LlvmIrEmitter emitter(module.get(), std::move(storage), &interner,
                        ir::PointerWidth::W64);

  std::move(emitter).emit();

  CHECK(!llvm::verifyModule(*module));

  std::string ir_str;
  llvm::raw_string_ostream os(ir_str);
  module->print(os, nullptr);

  CHECK(ir_str.find("atomicrmw") != std::string::npos);
  CHECK(ir_str.find("cmpxchg") != std::string::npos);
  CHECK(ir_str.find("getelementptr") != std::string::npos);
  CHECK(ir_str.find("extractvalue") != std::string::npos);
  CHECK(ir_str.find("insertvalue") != std::string::npos);
}

TEST_CASE("Emit ignores Drop markers") {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("marker_test", context);

  str::StringInterner interner(mem::page_size());
  ir::StorageBuilder builder;

  const ir::TypeIdx i32 = builder.primitive(ir::TypeTag::I32);
  const ir::ImmutableIdx one =
      builder.immutable({.type = i32, .data = {.i32_value = 1}});

  auto imm_op = [&](ir::ImmutableIdx imm, ir::TypeIdx ty) {
    return builder.operand(ir::Operand::from_immutable(imm, ty));
  };
  auto reg_op = [&](ir::RegisterIdx reg, ir::TypeIdx ty) {
    return builder.operand(ir::Operand::from_register(reg, ty));
  };

  ir::InstrSeq instrs;
  const ir::InstructionIdx inst_alloc =
      builder.instr({.op = ir::Opcode::Alloca,
                     .flags = {},
                     .dst = ir::RegisterIdx(0),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {imm_op(one, i32), 1}});
  instrs.push(inst_alloc);
  builder.reg({.type = i32, .def_idx = inst_alloc});
  // Drop is an ownership marker only; no code is emitted for it.
  ir::OperandSeq drop_args;
  drop_args.push(
      reg_op(ir::RegisterIdx(0), ir::primitive_idx(ir::TypeTag::Ptr)));
  instrs.push(builder.instr({.op = ir::Opcode::Drop,
                             .flags = {},
                             .dst = ir::RegisterIdx(base::kInvalidIdx),
                             .measure = ir::TypeIdx::invalid(),
                             .operands = drop_args.finish()}));
  const ir::InstructionIdx inst_ret =
      builder.instr({.op = ir::Opcode::Ret,
                     .flags = {},
                     .dst = ir::RegisterIdx(base::kInvalidIdx),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {}});
  instrs.push(inst_ret);
  const ir::BlockIdx block =
      builder.block({.instrs = instrs.finish(), .block_params = {}});
  builder.function({
      .meta = {.return_type = builder.primitive(ir::TypeTag::Void),
               .param_types = {},
               .name = interner.intern("markertest"),
               .path = str::kEmptyStringId,
               .kind = ir::SymbolKind::Foreign,
               .generics = ir::TypeIdxRange{}},
      .blocks = {block, 1},
  });

  ir::VerifiedStorage storage = std::move(builder).build().unwrap();
  LlvmIrEmitter emitter(module.get(), std::move(storage), &interner,
                        ir::PointerWidth::W64);

  std::move(emitter).emit();

  CHECK(!llvm::verifyModule(*module));

  std::string ir_str;
  llvm::raw_string_ostream os(ir_str);
  module->print(os, nullptr);

  CHECK(ir_str.find("drop") == std::string::npos);
}

}  // namespace codegen_llvm
