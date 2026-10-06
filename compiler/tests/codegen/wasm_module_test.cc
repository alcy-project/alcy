// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "codegen/wasm/module.h"
#include "codegen/wasm/writer.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"

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
    CHECK(payload[6] == 0x0B);      // The builder's closing end.
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

}  // namespace codegen::wasm
