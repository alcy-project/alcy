// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "codegen/backend.h"
#include "codegen/target.h"
#include "codegen/wasm/emit.h"
#include "codegen/wasm/module.h"
#include "codegen/wasm/writer.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/symbol_table.h"
#include "ir/type.h"

namespace codegen::wasm {

namespace {

u32 read_u32(std::span<const u8>& bytes) {
  u32 value = 0;
  u32 shift = 0;
  while (!bytes.empty()) {
    const u8 byte = bytes[0];
    bytes = bytes.subspan(1);
    value |= static_cast<u32>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      break;
    }
    shift += 7;
  }
  return value;
}

std::string_view read_name(std::span<const u8>& bytes) {
  const u32 size = read_u32(bytes);
  const std::string_view name(reinterpret_cast<const char*>(bytes.data()),
                              size);
  bytes = bytes.subspan(size);
  return name;
}

struct Section {
  u8 id = 0;
  std::span<const u8> payload;
};

std::vector<Section> read_sections(const std::vector<u8>& module) {
  std::vector<Section> sections;
  if (module.size() <= 8) {
    return sections;
  }
  std::span<const u8> bytes(module);
  bytes = bytes.subspan(8);  // Magic and version.
  while (!bytes.empty()) {
    const u8 id = bytes[0];
    bytes = bytes.subspan(1);
    const u32 size = read_u32(bytes);
    if (bytes.size() < size) {
      break;
    }
    sections.push_back(Section{id, bytes.first(size)});
    bytes = bytes.subspan(size);
  }
  return sections;
}

FuncBody add_one_body() {
  FuncBody body;
  body.code.byte(0x20);  // local.get
  body.code.u32_leb(0);
  body.code.byte(0x41);  // i32.const
  body.code.i32_leb(1);
  body.code.byte(0x6A);  // i32.add
  return body;
}

// The nth body in the code section. The section lists one body per
// defined function in wasm index order; the two WASI imports take no
// body, so the seven runtime functions come first and a module's own
// functions follow them.
std::span<const u8> code_body_at(const std::vector<u8>& module, u32 index) {
  for (const Section& section : read_sections(module)) {
    if (section.id != 10) {
      continue;
    }
    std::span<const u8> payload = section.payload;
    const u32 count = read_u32(payload);
    for (u32 i = 0; i <= index && i < count; ++i) {
      const u32 size = read_u32(payload);
      if (i == index) {
        return payload.first(size);
      }
      payload = payload.subspan(size);
    }
    return {};
  }
  return {};
}

// The seven defined runtime functions occupy the first bodies; a module
// with one own function finds that body here. Pinning the number keeps
// the test honest if the runtime's function count moves.
constexpr u32 FIRST_PROGRAM_BODY = 7;

// One verified free function: `fn not_bool() -> bool { ret !true }`.
// The operand is a literal, so the logical-not sequence is the only one
// the body can hold and the bytes the test looks for cannot belong to
// something else.
ir::VerifiedStorage not_bool_storage(ir::SymbolTable& strings) {
  ir::StorageBuilder builder;
  const ir::TypeIdx i1 = builder.primitive(ir::TypeTag::I1);
  const ir::ImmutableIdx on =
      builder.immutable({.type = i1, .data = {.i1_value = true}});
  const ir::OperandIdx arg =
      builder.operand(ir::Operand::from_immutable(on, i1));
  const ir::RegisterIdx result(0);
  const ir::InstructionIdx not_instr = builder.instr({
      .op = ir::Opcode::Not,
      .flags = {},
      .dst = result,
      .measure = ir::TypeIdx::invalid(),
      .operands = {arg, 1},
  });
  builder.reg({.type = i1, .def_idx = not_instr});
  const ir::OperandIdx ret_arg =
      builder.operand(ir::Operand::from_register(result, i1));
  (void)builder.instr({
      .op = ir::Opcode::Ret,
      .flags = {},
      .dst = ir::RegisterIdx::invalid(),
      .measure = ir::TypeIdx::invalid(),
      .operands = {ret_arg, 1},
  });
  const ir::BlockIdx block = builder.block({
      .instrs = ir::InstructionIdxRange{not_instr, 2},
      .block_params = {},
  });
  builder.function({
      .meta = {.return_type = i1,
               .param_types = {},
               .name = strings.intern("not_bool"),
               .path = str::EMPTY_STRING_ID,
               .kind = ir::SymbolKind::Free,
               .generics = {}},
      .blocks = {block, 1},
  });
  return std::move(builder).build().unwrap();
}

// Runs the whole backend on a wasm target, so a case checks the module
// the compiler would write rather than one the test assembles.
base::Result<std::vector<u8>, codegen::EmitError> emit_test_module(
    ir::VerifiedStorage storage,
    ir::SymbolTable& strings) {
  codegen::EmitRequest request{
      .storage = std::move(storage),
      .instr_spans = {},
      .instr_site_names = {},
      .strings = &strings,
      .bag = nullptr,
      .target =
          codegen::Target{"wasm32-unknown-emscripten", ir::PointerWidth::W32},
      .emit_entry = false,
      .freestanding = false,
      .optimize = false,
      .kind = codegen::OutputKind::Module,
      .profiler = nullptr,
  };
  return emit_module(std::move(request));
}

// Whether any function body in the code section holds `needle`.
bool any_body_contains(const std::vector<u8>& module,
                       std::span<const u8> needle) {
  for (const Section& section : read_sections(module)) {
    if (section.id != 10) {
      continue;
    }
    std::span<const u8> payload = section.payload;
    const u32 count = read_u32(payload);
    for (u32 i = 0; i < count; ++i) {
      const u32 size = read_u32(payload);
      const std::span<const u8> body = payload.first(size);
      if (std::search(body.begin(), body.end(), needle.begin(), needle.end()) !=
          body.end()) {
        return true;
      }
      payload = payload.subspan(size);
    }
  }
  return false;
}

}  // namespace

TEST_CASE("A module lays its sections in the format's order") {
  ModuleBuilder builder;
  const u32 type = builder.add_type(FuncType{{ValType::I32}, {ValType::I32}});
  const u32 proc_exit =
      builder.add_import("wasi_snapshot_preview1", "proc_exit",
                         builder.add_type(FuncType{{ValType::I32}, {}}));
  CHECK(proc_exit == 0);
  builder.set_memory(1);
  const u32 function = builder.add_function(type);
  builder.set_body(function, add_one_body());
  builder.export_function("f", function);
  builder.export_memory("memory");
  builder.add_global(ValType::I32, true, 0x1000);
  builder.add_data(std::array<u8, 2>{0x68, 0x69});
  builder.set_start(function);

  const std::vector<u8> module = builder.finish();
  CHECK(module.size() > 8);
  CHECK(module[0] == 0x00);
  CHECK(module[1] == 0x61);
  CHECK(module[2] == 0x73);
  CHECK(module[3] == 0x6D);
  CHECK(module[4] == 0x01);

  std::vector<Section> sections = read_sections(module);
  // Type, import, function, memory, global, export, start, code, data.
  CHECK(sections.size() == 9);
  if (sections.size() != 9) {
    return;
  }
  CHECK(sections[0].id == 1);
  CHECK(sections[1].id == 2);
  CHECK(sections[2].id == 3);
  CHECK(sections[3].id == 5);
  CHECK(sections[4].id == 6);
  CHECK(sections[5].id == 7);
  CHECK(sections[6].id == 8);
  CHECK(sections[7].id == 10);
  CHECK(sections[8].id == 11);

  {
    // Two types: (i32)->i32 and (i32)->().
    std::span<const u8> payload = sections[0].payload;
    CHECK(read_u32(payload) == 2);
    CHECK(payload[0] == 0x60);
    payload = payload.subspan(1);
    CHECK(read_u32(payload) == 1);
    CHECK(payload[0] == static_cast<u8>(ValType::I32));
    payload = payload.subspan(1);
    CHECK(read_u32(payload) == 1);
    CHECK(payload[0] == static_cast<u8>(ValType::I32));
  }
  {
    std::span<const u8> payload = sections[1].payload;
    CHECK(read_u32(payload) == 1);
    CHECK(read_name(payload) == "wasi_snapshot_preview1");
    CHECK(read_name(payload) == "proc_exit");
    CHECK(payload[0] == 0x00);
    payload = payload.subspan(1);
    CHECK(read_u32(payload) == 1);
  }
  {
    std::span<const u8> payload = sections[2].payload;
    CHECK(read_u32(payload) == 1);
    CHECK(read_u32(payload) == 0);
  }
  {
    std::span<const u8> payload = sections[3].payload;
    CHECK(read_u32(payload) == 1);  // One memory.
    CHECK(payload[0] == 0x00);      // No maximum.
    payload = payload.subspan(1);
    CHECK(read_u32(payload) == 1);  // One page.
  }
  {
    std::span<const u8> payload = sections[4].payload;
    CHECK(read_u32(payload) == 1);
    CHECK(payload[0] == static_cast<u8>(ValType::I32));
    CHECK(payload[1] == 0x01);
    CHECK(payload[2] == 0x41);
  }
  {
    std::span<const u8> payload = sections[5].payload;
    CHECK(read_u32(payload) == 2);
    CHECK(read_name(payload) == "f");
    CHECK(payload[0] == 0x00);
    payload = payload.subspan(1);
    CHECK(read_u32(payload) == 1);
    CHECK(read_name(payload) == "memory");
    CHECK(payload[0] == 0x02);
  }
  CHECK(read_u32(sections[6].payload) == 1);
  {
    std::span<const u8> payload = sections[7].payload;
    CHECK(read_u32(payload) == 1);
    const u32 size = read_u32(payload);
    CHECK(payload.size() == size);
    CHECK(read_u32(payload) == 0);  // No locals; the parameter is local 0.
    CHECK(payload[0] == 0x20);      // local.get 0
    CHECK(payload.size() == 6);
    CHECK(payload[5] == 0x0B);  // The builder's closing end.
  }
  {
    std::span<const u8> payload = sections[8].payload;
    CHECK(read_u32(payload) == 1);
    CHECK(payload[0] == 0x00);
    CHECK(payload[1] == 0x41);
    CHECK(payload[2] == 0x00);  // Offset zero.
    CHECK(payload[3] == 0x0B);
    CHECK(read_u32(payload = payload.subspan(4)) == 2);
    CHECK(payload[0] == 'h');
    CHECK(payload[1] == 'i');
  }
}

TEST_CASE("Imports take the first function indices") {
  ModuleBuilder builder;
  const u32 exits = builder.add_type(FuncType{{ValType::I32}, {}});
  const u32 import =
      builder.add_import("wasi_snapshot_preview1", "proc_exit", exits);
  const u32 defined = builder.add_function(exits);
  CHECK(import == 0);
  CHECK(defined == 1);
  FuncBody body;
  builder.set_body(defined, std::move(body));
  builder.set_start(defined);

  const std::vector<u8> module = builder.finish();
  std::vector<Section> sections = read_sections(module);
  CHECK(sections.size() == 5);  // Type, import, function, start, code.
  if (sections.size() != 5) {
    return;
  }
  CHECK(sections[3].id == 8);
  CHECK(read_u32(sections[3].payload) == 1);
}

TEST_CASE("A table holds the functions its elements name") {
  ModuleBuilder builder;
  const u32 type = builder.add_type(FuncType{{}, {ValType::I32}});
  const u32 function = builder.add_function(type);
  builder.set_body(function, FuncBody{});
  builder.set_table(3);
  const u32 entries[1] = {function};
  builder.add_element(2, entries);

  const std::vector<u8> module = builder.finish();
  std::vector<Section> sections = read_sections(module);
  // Type, function, table, element, code.
  CHECK(sections.size() == 5);
  if (sections.size() != 5) {
    return;
  }
  CHECK(sections[0].id == 1);
  CHECK(sections[1].id == 3);
  CHECK(sections[2].id == 4);
  CHECK(sections[3].id == 9);
  CHECK(sections[4].id == 10);

  std::span<const u8> table = sections[2].payload;
  CHECK(read_u32(table) == 1);  // One table.
  CHECK(table[0] == 0x70);      // funcref.
  CHECK(table[1] == 0x00);      // No maximum.
  std::span<const u8> limits = table.subspan(2);
  CHECK(read_u32(limits) == 3);

  std::span<const u8> element = sections[3].payload;
  CHECK(read_u32(element) == 1);  // One segment.
  CHECK(element[0] == 0x00);      // Active, table 0.
  CHECK(element[1] == 0x41);      // i32.const
  std::span<const u8> offset = element.subspan(2);
  CHECK(read_u32(offset) == 2);
}

TEST_CASE("Data segments are aligned and keep their offsets") {
  ModuleBuilder builder;
  builder.set_memory(1);
  const u32 first =
      builder.add_data(std::array<u8, 5>{0x01, 0x02, 0x03, 0x04, 0x05});
  const u32 second = builder.add_data(std::array<u8, 1>{0x06}, 4);
  CHECK(first == 0);
  CHECK(second == 8);

  const std::vector<u8> module = builder.finish();
  std::vector<Section> sections = read_sections(module);
  CHECK(sections.size() == 2);  // Memory and data.
  if (sections.size() != 2) {
    return;
  }
  std::span<const u8> payload = sections[1].payload;
  CHECK(read_u32(payload) == 2);
  payload = payload.subspan(4);  // Active segment, i32.const 0, end.
  CHECK(read_u32(payload) == 5);
}

TEST_CASE("A bool complement is the logical not") {
  // A canonical bool is one 0/1 word. Emitting `~x` as an all-ones
  // complement left `!true` as 0xFFFFFFFF, which every branch still read
  // as true; the emitter has to write the logical not instead.
  ir::SymbolTable strings{16};
  base::Result<std::vector<u8>, codegen::EmitError> emitted =
      emit_test_module(not_bool_storage(strings), strings);
  CHECK(emitted.is_ok());
  if (emitted.is_err()) {
    return;
  }
  const std::vector<u8> module = std::move(emitted).unwrap();
  const std::span<const u8> body = code_body_at(module, FIRST_PROGRAM_BODY);
  CHECK(!body.empty());
  // `i32.const 1; i32.eqz`: the true operand and the logical not.
  const std::array<u8, 3> logical_not{0x41, 0x01, 0x45};
  CHECK(std::search(body.begin(), body.end(), logical_not.begin(),
                    logical_not.end()) != body.end());
  // `i32.const -1; i32.xor`: the complement this case exists to reject.
  const std::array<u8, 3> complement{0x41, 0x7F, 0x73};
  CHECK(std::search(body.begin(), body.end(), complement.begin(),
                    complement.end()) == body.end());
}

TEST_CASE("A failed wasm allocation panics instead of trapping") {
  // The bump allocator grows the memory and used to drop the result: a
  // failed grow still returned the block, and the caller's first write
  // trapped somewhere else. The result is checked and reported now.
  ir::SymbolTable strings{16};
  base::Result<std::vector<u8>, codegen::EmitError> emitted =
      emit_test_module(not_bool_storage(strings), strings);
  CHECK(emitted.is_ok());
  if (emitted.is_err()) {
    return;
  }
  const std::vector<u8> module = std::move(emitted).unwrap();
  // `memory.grow 0; i32.const -1; i32.eq`: grow reports failure as -1.
  const std::array<u8, 5> grow_check{0x40, 0x00, 0x41, 0x7F, 0x46};
  CHECK(any_body_contains(module, grow_check));
  // `call 5` is the runtime's panic and `unreachable` ends the branch.
  const std::array<u8, 3> panic_call{0x10, 0x05, 0x00};
  CHECK(any_body_contains(module, panic_call));
  // And the message the panic writes is part of the module's data.
  constexpr std::string_view MESSAGE = "allocation failed";
  const std::span<const u8> message(reinterpret_cast<const u8*>(MESSAGE.data()),
                                    MESSAGE.size());
  CHECK(std::search(module.begin(), module.end(), message.begin(),
                    message.end()) != module.end());
}

}  // namespace codegen::wasm
