// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/text_dump.h"

#include <array>
#include <string>
#include <utility>

#include "diag/span.h"
#include "doctest/doctest.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_interner.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "ir/write_input.h"

namespace ir {

namespace {

// Two parameters and a branch, so the dump covers block parameters,
// block labels, a terminator with block operands, and a binary op.
Storage branch_storage(str::StringInterner& strings) {
  StorageBuilder builder;
  const TypeIdx i1 = builder.primitive(TypeTag::I1);
  const TypeIdx i32 = builder.primitive(TypeTag::I32);
  const TypeIdx void_ty = builder.primitive(TypeTag::Void);

  const BlockIdx b0(0);
  const BlockIdx b1(1);
  const BlockIdx b2(2);
  builder.block({});
  builder.block({});
  builder.block({});

  const RegisterIdx v0(0);
  const RegisterIdx v1(1);
  const RegisterIdx v2(2);
  const RegisterIdx v3(3);
  builder.block_param({.type = i1, .reg = v0});
  builder.block_param({.type = i32, .reg = v1});
  builder.block_param({.type = i32, .reg = v2});

  const OperandIdx cond = builder.operand(Operand::from_register(v0, i1));
  builder.operand(Operand::from_block(b1, void_ty));
  builder.operand(Operand::from_block(b2, void_ty));
  const InstructionIdx cond_br = builder.instr({
      .op = Opcode::CondBr,
      .flags = {},
      .dst = RegisterIdx::invalid(),
      .measure = TypeIdx::invalid(),
      .operands = {cond, 3},
  });

  const OperandIdx ret_v1 = builder.operand(Operand::from_register(v1, i32));
  const InstructionIdx ret_b1 = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx::invalid(),
      .measure = TypeIdx::invalid(),
      .operands = {ret_v1, 1},
  });

  const OperandIdx add_lhs = builder.operand(Operand::from_register(v1, i32));
  builder.operand(Operand::from_register(v2, i32));
  const InstructionIdx add = builder.instr({
      .op = Opcode::IntAdd,
      .flags = {},
      .dst = v3,
      .measure = TypeIdx::invalid(),
      .operands = {add_lhs, 2},
  });
  const OperandIdx ret_v3 = builder.operand(Operand::from_register(v3, i32));
  builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx::invalid(),
      .measure = TypeIdx::invalid(),
      .operands = {ret_v3, 1},
  });

  builder.reg({.type = i1, .def_idx = InstructionIdx::invalid()});
  builder.reg({.type = i32, .def_idx = InstructionIdx::invalid()});
  builder.reg({.type = i32, .def_idx = InstructionIdx::invalid()});
  builder.reg({.type = i32, .def_idx = add});

  builder.set_block_instrs(b0, {cond_br, 1});
  builder.set_block_params(b0, {BlockParamIdx(0), 3});
  builder.set_block_instrs(b1, {ret_b1, 1});
  builder.set_block_instrs(b2, {add, 2});

  TypeSeq params;
  params.push(builder.ref_type(i1));
  params.push(builder.ref_type(i32));
  params.push(builder.ref_type(i32));
  const FunctionMeta meta{
      .return_type = i32,
      .param_types = params.finish(),
      .name = strings.intern("choose"),
      .path = str::EMPTY_STRING_ID,
      .kind = SymbolKind::Free,
      .generics = TypeIdxRange{},
  };
  builder.function({.meta = meta, .blocks = {b0, 3}});
  return std::move(builder).build().unwrap().unwrap();
}

// An alloca with a name and spans, so the dump covers the comment parts.
Storage named_alloca_storage(str::StringInterner& strings) {
  StorageBuilder builder;
  const TypeIdx i32 = builder.primitive(TypeTag::I32);

  const ImmutableIdx one =
      builder.immutable({.type = i32, .data = {.i32_value = 1}});
  const ImmutableIdx five =
      builder.immutable({.type = i32, .data = {.i32_value = 5}});
  const OperandIdx one_op = builder.operand(Operand::from_immutable(one, i32));
  const OperandIdx five_op =
      builder.operand(Operand::from_immutable(five, i32));

  const RegisterIdx v0(0);
  const RegisterIdx v1(1);
  const InstructionIdx alloc = builder.instr({
      .op = Opcode::Alloca,
      .flags = {},
      .dst = v0,
      .measure = TypeIdx::invalid(),
      .operands = {one_op, 1},
  });
  const OperandIdx alloc_ptr = builder.operand(Operand::from_register(v0, i32));
  builder.operand(Operand::from_register(v0, i32));
  builder.instr({
      .op = Opcode::Store,
      .flags = {},
      .dst = RegisterIdx::invalid(),
      .measure = TypeIdx::invalid(),
      .operands = {five_op, 2},
  });
  const InstructionIdx load = builder.instr({
      .op = Opcode::Load,
      .flags = {},
      .dst = v1,
      .measure = TypeIdx::invalid(),
      .operands = {alloc_ptr, 1},
  });
  const OperandIdx ret_v1 = builder.operand(Operand::from_register(v1, i32));
  builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx::invalid(),
      .measure = TypeIdx::invalid(),
      .operands = {ret_v1, 1},
  });

  builder.reg({.type = i32, .def_idx = alloc});
  builder.reg({.type = i32, .def_idx = load});
  const BlockIdx b0 = builder.block({.instrs = {alloc, 4}, .block_params = {}});
  const FunctionMeta meta{
      .return_type = i32,
      .param_types = {},
      .name = strings.intern("main"),
      .path = str::EMPTY_STRING_ID,
      .kind = SymbolKind::Free,
      .generics = TypeIdxRange{},
  };
  builder.function({.meta = meta, .blocks = {b0, 1}});
  return std::move(builder).build().unwrap().unwrap();
}

}  // namespace

TEST_CASE("The text view names blocks and values by their storage indices") {
  str::StringInterner strings;
  const Storage storage = branch_storage(strings);

  const WriteInput input{
      .storage = &storage,
      .strings = &strings,
      .instr_spans = {},
      .files = {},
      .addr_names = {},
      .prelude_functions = 0,
      .width = PointerWidth::W64,
  };
  const std::string text = write_text(input);
  const std::string want =
      "// alcy ir, format 1, pointer width 64\n"
      "\n"
      "fn choose(v0: bool, v1: i32, v2: i32) -> i32 {  // #0\n"
      "b0:\n"
      "  condbr v0, b1, b2\n"
      "b1:\n"
      "  ret v1\n"
      "b2:\n"
      "  v3 = v1 + v2\n"
      "  ret v3\n"
      "}\n";
  CHECK(text == want);
}

TEST_CASE("Address names and spans ride as comments") {
  str::StringInterner strings;
  const Storage storage = named_alloca_storage(strings);

  const std::array<std::string_view, 1> names{"main.al"};
  const std::array<AddrName, 1> addr_names{
      AddrName{RegisterIdx(0), "x", /*is_param=*/true, /*is_capture=*/false}};
  const std::array<diag::Span, 4> spans{
      diag::Span{.file = 0, .offset = 7, .length = 1},
      diag::Span{.file = 0, .offset = 10, .length = 6},
      diag::Span{.file = 0, .offset = 20, .length = 2},
      diag::Span{.file = 0, .offset = 25, .length = 6},
  };

  const WriteInput input{
      .storage = &storage,
      .strings = &strings,
      .instr_spans = spans,
      .files = FileTable{.names = names, .hashes = {}},
      .addr_names = addr_names,
      .prelude_functions = 0,
      .width = PointerWidth::W64,
  };
  const std::string text = write_text(input);
  const std::string want =
      "// alcy ir, format 1, pointer width 64\n"
      "\n"
      "fn main() -> i32 {  // #0\n"
      "b0:\n"
      "  v0 = alloca i32  // x (param)  main.al:7+1\n"
      "  *v0 = 5  // main.al:10+6\n"
      "  v1 = *v0  // main.al:20+2\n"
      "  ret v1  // main.al:25+6\n"
      "}\n";
  CHECK(text == want);
}

TEST_CASE("The preamble declares composites and externs") {
  StorageBuilder builder;
  str::StringInterner strings;
  const TypeIdx i32 = builder.primitive(TypeTag::I32);
  const TypeIdx ptr = builder.primitive(TypeTag::Ptr);
  const TypeIdx u32 = builder.primitive(TypeTag::U32);
  const TypeIdx void_ty = builder.primitive(TypeTag::Void);

  TypeSeq fields;
  fields.push(builder.ref_type(i32));
  fields.push(builder.ref_type(i32));
  const TypeIdx point = builder.struct_type(strings.intern("Point"),
                                            fields.finish(), TypeIdxRange{});

  EnumVariantTypeSeq variants;
  variants.push(builder.enum_variant(strings.intern("Circle"), {i32, 1}));
  variants.push(builder.enum_variant(strings.intern("Rect"), {}));
  const TypeIdx shape = builder.enum_type(strings.intern("Shape"),
                                          variants.finish(), TypeIdxRange{});

  TypeSeq params;
  params.push(builder.ref_type(ptr));
  params.push(builder.ref_type(u32));
  builder.external_function({
      .meta = {.return_type = void_ty,
               .param_types = params.finish(),
               .name = strings.intern("alcy_print"),
               .path = str::EMPTY_STRING_ID,
               .kind = SymbolKind::Foreign,
               .generics = TypeIdxRange{}},
      .calling_conv = CallingConvention::C,
  });

  const Storage storage = std::move(builder).build().unwrap().unwrap();
  const WriteInput input{
      .storage = &storage,
      .strings = &strings,
      .instr_spans = {},
      .files = {},
      .addr_names = {},
      .prelude_functions = 0,
      .width = PointerWidth::W64,
  };
  const std::string text = write_text(input);
  const std::string want = fmt::format(
      "// alcy ir, format 1, pointer width 64\n"
      "\n"
      "struct Point(i32, i32)  // #{}\n"
      "enum Shape {{ Circle(i32), Rect }}  // #{}\n"
      "extern fn alcy_print(ptr, u32)  // #0\n"
      "\n",
      point.idx, shape.idx);
  CHECK(text == want);
}

}  // namespace ir
