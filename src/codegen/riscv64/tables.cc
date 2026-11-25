// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/riscv64/tables.h"

#include <vector>

#include "codegen/object/elf64.h"

namespace codegen::riscv64 {
namespace {

constexpr u64 ENTRY_HEADER = 32;
constexpr u64 TERMINATOR = 32;

u64 table_size(const std::vector<BacktraceEntry>& entries) {
  u64 size = 0;
  for (const BacktraceEntry& entry : entries) {
    size += ENTRY_HEADER + ((entry.text.size() + 7) & ~7ull);
  }
  return size + TERMINATOR;
}

void put_u64(std::vector<u8>& bytes, const u64 value) {
  for (u32 at = 0; at < 8; ++at) {
    bytes.push_back(static_cast<u8>(value >> (at * 8)));
  }
}

void put_entry(std::vector<u8>& bytes, const BacktraceEntry& entry) {
  put_u64(bytes, static_cast<u64>(entry.offset));
  put_u64(bytes, entry.size);
  put_u64(bytes, entry.text.size());
  put_u64(bytes, 0);
  bytes.insert(bytes.end(), entry.text.begin(), entry.text.end());
  while (bytes.size() % 8 != 0) {
    bytes.push_back(0);
  }
}

void put_terminator(std::vector<u8>& bytes) {
  put_u64(bytes, 0xffffffffffffffffull);
  for (u32 i = 0; i < 3; ++i) {
    put_u64(bytes, 0);
  }
}

void add_symbol(object::Object& object,
                const char* name,
                const u64 offset,
                const u64 size) {
  object.symbols.push_back(object::Symbol{
      .name = name,
      .section = 0,
      .offset = offset,
      .size = size,
      .bind = object::SymbolBind::Local,
      .type = object::SymbolType::Object,
  });
}

}  // namespace

u64 BacktraceTables::functions_size() const {
  return table_size(functions);
}

u64 BacktraceTables::sites_size() const {
  return table_size(sites);
}

void emit_tables(const BacktraceTables& tables,
                 const u64 functions_start,
                 const u64 sites_start,
                 std::vector<u8>& bytes,
                 object::Object& object) {
  if (!tables.enabled) {
    return;
  }
  while (bytes.size() < functions_start) {
    bytes.push_back(0);
  }
  for (const BacktraceEntry& entry : tables.functions) {
    put_entry(bytes, entry);
  }
  put_terminator(bytes);
  add_symbol(object, "alcy_bt_funcs_array", functions_start,
             bytes.size() - functions_start);

  while (bytes.size() < sites_start) {
    bytes.push_back(0);
  }
  for (const BacktraceEntry& entry : tables.sites) {
    put_entry(bytes, entry);
  }
  put_terminator(bytes);
  add_symbol(object, "alcy_bt_sites_array", sites_start,
             bytes.size() - sites_start);
}

}  // namespace codegen::riscv64
