// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/wasm/module.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codegen/wasm/writer.h"
#include "debug/dcheck.h"
#include "fpag/base/numeric.h"
#include "ir/storage.h"

namespace codegen::wasm {
namespace {

// The section ids, in the order the format requires them.
constexpr u8 TYPE_SECTION = 1;
constexpr u8 IMPORT_SECTION = 2;
constexpr u8 FUNCTION_SECTION = 3;
constexpr u8 MEMORY_SECTION = 5;
constexpr u8 GLOBAL_SECTION = 6;
constexpr u8 EXPORT_SECTION = 7;
constexpr u8 START_SECTION = 8;
constexpr u8 CODE_SECTION = 10;
constexpr u8 DATA_SECTION = 11;

// Export kinds.
constexpr u8 EXPORT_FUNCTION = 0x00;
constexpr u8 EXPORT_MEMORY = 0x02;

void write_val_type(BinaryWriter& out, ValType type) {
  out.byte(static_cast<u8>(type));
}

void write_type_section(const std::vector<FuncType>& types, BinaryWriter& out) {
  out.u32_leb(static_cast<u32>(types.size()));
  for (const FuncType& type : types) {
    out.byte(0x60);
    out.u32_leb(static_cast<u32>(type.params.size()));
    for (ValType param : type.params) {
      write_val_type(out, param);
    }
    out.u32_leb(static_cast<u32>(type.results.size()));
    for (ValType result : type.results) {
      write_val_type(out, result);
    }
  }
}

void write_code_entry(const FuncBody& body, BinaryWriter& out) {
  BinaryWriter locals;
  locals.u32_leb(static_cast<u32>(body.locals.size()));
  for (ValType local : body.locals) {
    locals.u32_leb(1);
    write_val_type(locals, local);
  }
  BinaryWriter entry;
  entry.bytes(locals.bytes());
  entry.bytes(body.code.bytes());
  entry.byte(0x0B);  // The function's closing `end`.
  out.u32_leb(static_cast<u32>(entry.size()));
  out.bytes(entry.bytes());
}

}  // namespace

u32 ModuleBuilder::add_type(const FuncType& type) {
  for (u32 i = 0; i < static_cast<u32>(types_.size()); ++i) {
    if (types_[i] == type) {
      return i;
    }
  }
  types_.push_back(type);
  return static_cast<u32>(types_.size()) - 1;
}

u32 ModuleBuilder::add_import(std::string_view module,
                              std::string_view field,
                              u32 type) {
  DCHECK(type < types_.size());
  imports_.push_back(Import{std::string(module), std::string(field), type});
  return static_cast<u32>(imports_.size()) - 1;
}

u32 ModuleBuilder::add_function(u32 type) {
  DCHECK(type < types_.size());
  functions_.push_back(
      Defined{.type = type, .body = FuncBody{}, .has_body = false});
  // Imported functions come first, so a defined function's index is its
  // own position shifted past them.
  return static_cast<u32>(imports_.size() + functions_.size()) - 1;
}

void ModuleBuilder::set_body(u32 function, FuncBody body) {
  DCHECK(function >= imports_.size());
  const u32 index = function - static_cast<u32>(imports_.size());
  DCHECK(index < functions_.size());
  functions_[index].body = std::move(body);
  functions_[index].has_body = true;
}

void ModuleBuilder::export_function(std::string_view name, u32 function) {
  DCHECK(function < imports_.size() + functions_.size());
  exports_.push_back(Export{std::string(name), EXPORT_FUNCTION, function});
}

void ModuleBuilder::export_memory(std::string_view name) {
  exports_.push_back(Export{std::string(name), EXPORT_MEMORY, 0});
}

void ModuleBuilder::set_memory(u32 min_pages) {
  has_memory_ = true;
  memory_min_pages_ = min_pages;
}

u32 ModuleBuilder::add_global(ValType type, bool is_mutable, i32 init) {
  globals_.push_back(Global{type, is_mutable, init});
  return static_cast<u32>(globals_.size()) - 1;
}

u32 ModuleBuilder::add_data(std::span<const u8> bytes, u32 align) {
  DCHECK(has_memory_);
  const u32 offset = static_cast<u32>(ir::align_up(data_end_, align));
  data_end_ = offset + static_cast<u32>(bytes.size());
  segments_.push_back(
      Segment{offset, std::vector<u8>(bytes.begin(), bytes.end())});
  return offset;
}

void ModuleBuilder::set_start(u32 function) {
  has_start_ = true;
  start_ = function;
}

std::vector<u8> ModuleBuilder::finish() const {
  BinaryWriter out;
  constexpr u8 MAGIC[4] = {0x00, 0x61, 0x73, 0x6D};  // "\0asm"
  out.bytes(MAGIC);
  constexpr u8 VERSION[4] = {0x01, 0x00, 0x00, 0x00};
  out.bytes(VERSION);

  if (!types_.empty()) {
    BinaryWriter payload;
    write_type_section(types_, payload);
    write_section(out, TYPE_SECTION, payload);
  }
  if (!imports_.empty()) {
    BinaryWriter payload;
    payload.u32_leb(static_cast<u32>(imports_.size()));
    for (const Import& import : imports_) {
      payload.name(import.module);
      payload.name(import.field);
      payload.byte(0x00);  // A function import.
      payload.u32_leb(import.type);
    }
    write_section(out, IMPORT_SECTION, payload);
  }
  if (!functions_.empty()) {
    BinaryWriter payload;
    payload.u32_leb(static_cast<u32>(functions_.size()));
    for (const Defined& function : functions_) {
      payload.u32_leb(function.type);
    }
    write_section(out, FUNCTION_SECTION, payload);
  }
  if (has_memory_) {
    BinaryWriter payload;
    payload.u32_leb(1);  // One memory.
    payload.byte(0x00);  // No maximum.
    payload.u32_leb(memory_min_pages_);
    write_section(out, MEMORY_SECTION, payload);
  }
  if (!globals_.empty()) {
    BinaryWriter payload;
    payload.u32_leb(static_cast<u32>(globals_.size()));
    for (const Global& global : globals_) {
      write_val_type(payload, global.type);
      payload.byte(global.mutable_ ? 0x01 : 0x00);
      payload.byte(0x41);  // i32.const
      payload.i32_leb(global.init);
      payload.byte(0x0B);
    }
    write_section(out, GLOBAL_SECTION, payload);
  }
  if (!exports_.empty()) {
    BinaryWriter payload;
    payload.u32_leb(static_cast<u32>(exports_.size()));
    for (const Export& export_ : exports_) {
      payload.name(export_.name);
      payload.byte(export_.kind);
      payload.u32_leb(export_.index);
    }
    write_section(out, EXPORT_SECTION, payload);
  }
  if (has_start_) {
    BinaryWriter payload;
    payload.u32_leb(start_);
    write_section(out, START_SECTION, payload);
  }
  if (!functions_.empty()) {
    BinaryWriter payload;
    payload.u32_leb(static_cast<u32>(functions_.size()));
    for (const Defined& function : functions_) {
      DCHECK(function.has_body);
      write_code_entry(function.body, payload);
    }
    write_section(out, CODE_SECTION, payload);
  }
  if (!segments_.empty()) {
    BinaryWriter payload;
    payload.u32_leb(static_cast<u32>(segments_.size()));
    for (const Segment& segment : segments_) {
      payload.byte(0x00);  // Active, memory 0.
      payload.byte(0x41);  // i32.const
      payload.i32_leb(static_cast<i32>(segment.offset));
      payload.byte(0x0B);
      payload.u32_leb(static_cast<u32>(segment.bytes.size()));
      payload.bytes(segment.bytes);
    }
    write_section(out, DATA_SECTION, payload);
  }
  return std::vector<u8>(out.bytes().begin(), out.bytes().end());
}

}  // namespace codegen::wasm
