// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <vector>

#include "fpag/base/numeric.h"

namespace codegen::object {

// One section's bytes as the emitter laid them out. The writer places the
// sections in the order given and appends the symbol, string, and section
// name tables a relocatable object needs; a caller never spells those.
struct Section {
  std::string name;
  std::vector<u8> bytes;
  // Alignment the section start is rounded up to, in bytes.
  u64 align = 1;
  bool exec = false;
  bool write = false;
  bool alloc = false;
  // False for a section the linker reserves without copying, which is
  // how `.bss` is spelled; every section an emitter fills is true.
  bool bits = true;
};

// How a relocation reaches the symbol it names. `Abs64` writes the
// address itself, for a pointer stored in data; the two `Pc` forms write
// the distance from the next instruction, which is what a direct call
// and a rip-relative reference read.
enum class RelocKind : u8 { Abs64, Pc32, Plt32 };

struct Relocation {
  // The section that carries the patched bytes, and the offset inside it.
  u32 section = 0;
  u64 offset = 0;
  RelocKind kind = RelocKind::Pc32;
  // The symbol the patch reaches, spelled as the object's own `Symbol`.
  std::string symbol;
  i64 addend = 0;
};

enum class SymbolBind : u8 { Local, Global };
enum class SymbolType : u8 { Notype, Object, Func };

struct Symbol {
  std::string name;
  // The section the symbol names, and the offset inside it.
  u32 section = 0;
  u64 offset = 0;
  u64 size = 0;
  SymbolBind bind = SymbolBind::Local;
  SymbolType type = SymbolType::Notype;
};

// One relocatable object's contents, ready to write.
struct Object {
  std::vector<Section> sections;
  std::vector<Symbol> symbols;
  std::vector<Relocation> relocations;
};

// Writes a relocatable ELF64 for x86-64. The symbol order is the
// table's own: section symbols for the sections relocations touch, then
// the locals, then the globals, and a relocation resolves the name it
// carries against that table. A name no symbol carries is an emitter
// bug, not an input the reader can fix.
[[nodiscard]] std::vector<u8> write_elf64_x86_64(const Object& object);

}  // namespace codegen::object
