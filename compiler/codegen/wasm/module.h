// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "codegen/wasm/writer.h"
#include "fpag/base/numeric.h"

namespace codegen::wasm {

// One function signature, as the type section spells it.
struct FuncType {
  std::vector<ValType> params;
  std::vector<ValType> results;

  [[nodiscard]] bool operator==(const FuncType&) const = default;
};

// A defined function's body: the locals after its parameters, and the
// instructions. The builder appends the function's closing `end`, so a
// body never has to remember it.
struct FuncBody {
  std::vector<ValType> locals;
  BinaryWriter code;
};

// Assembles a wasm module: the sections in the order the format
// requires, the index spaces the emitter names, and the lengths that
// precede their payloads. Imports come before defined functions in the
// function index space, so an emitter that wants a stable index for its
// own functions adds the runtime imports first.
class ModuleBuilder {
 public:
  // A deduplicated signature; returns its type index.
  u32 add_type(const FuncType& type);

  // An imported function; returns its function index.
  u32 add_import(std::string_view module, std::string_view field, u32 type);

  // A defined function with `type`; returns its function index.
  u32 add_function(u32 type);

  // Fills the body of a defined function.
  void set_body(u32 function, FuncBody body);

  // Exports the function or the memory under `name`.
  void export_function(std::string_view name, u32 function);
  void export_memory(std::string_view name);

  // The module's one memory. `min_pages` is 64 KiB each.
  void set_memory(u32 min_pages);

  // The module's one table, of funcref, and the functions its entries
  // hold from `offset`. An indirect call names a table index, so a
  // function value is the index its entry lives at.
  void set_table(u32 min_size);
  void add_element(u32 offset, std::span<const u32> functions);

  // A global with an i32 initial value; returns its global index.
  u32 add_global(ValType type, bool is_mutable, i32 init);

  // A data segment, placed at the next `align`-aligned offset; returns
  // that offset. The active segment is stored into the memory the module
  // declares, so `set_memory` comes first.
  u32 add_data(std::span<const u8> bytes, u32 align = 1);

  // The function the module starts in, if any.
  void set_start(u32 function);

  // Serializes every section. Every defined function needs a body.
  [[nodiscard]] std::vector<u8> finish() const;

 private:
  struct Import {
    std::string module;
    std::string field;
    u32 type = 0;
  };
  struct Defined {
    u32 type = 0;
    FuncBody body;
    bool has_body = false;
  };
  struct Global {
    ValType type = ValType::I32;
    bool mutable_ = false;
    i32 init = 0;
  };
  struct Segment {
    u32 offset = 0;
    std::vector<u8> bytes;
  };
  struct Export {
    std::string name;
    u8 kind = 0;
    u32 index = 0;
  };

  struct Element {
    u32 offset = 0;
    std::vector<u32> functions;
  };
  std::vector<FuncType> types_;
  std::vector<Import> imports_;
  std::vector<Defined> functions_;
  std::vector<Global> globals_;
  std::vector<Segment> segments_;
  std::vector<Export> exports_;
  u32 memory_min_pages_ = 0;
  bool has_memory_ = false;
  u32 table_min_size_ = 0;
  bool has_table_ = false;
  std::vector<Element> elements_;
  u32 data_end_ = 0;
  u32 start_ = 0;
  bool has_start_ = false;
};

}  // namespace codegen::wasm
