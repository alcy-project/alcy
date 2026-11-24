// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/object/elf64.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "debug/check.h"
#include "debug/dcheck.h"

namespace codegen::object {

namespace {

constexpr u16 ET_REL = 1;
constexpr u32 SHT_PROGBITS = 1;
constexpr u32 SHT_SYMTAB = 2;
constexpr u32 SHT_STRTAB = 3;
constexpr u32 SHT_RELA = 4;
constexpr u32 SHT_NOBITS = 8;
constexpr u64 SHF_WRITE = 1;
constexpr u64 SHF_ALLOC = 2;
constexpr u64 SHF_EXECINSTR = 4;
constexpr u32 R_X86_64_64 = 1;
constexpr u32 R_X86_64_PC32 = 2;
constexpr u32 R_X86_64_PLT32 = 4;
// RISC-V relocation numbers used by `reloc_type`. CALL and CALL_PLT
// reach a symbol through auipc+jalr; 64 is an absolute address.
constexpr u32 R_RISCV_64 = 2;
constexpr u32 R_RISCV_CALL = 18;
constexpr u32 R_RISCV_CALL_PLT = 19;
constexpr u64 EH_SIZE = 64;
constexpr u64 SH_SIZE = 64;
constexpr u64 SYM_SIZE = 24;
constexpr u64 RELA_SIZE = 24;

void put16(std::vector<u8>& out, const u16 value) {
  out.push_back(static_cast<u8>(value));
  out.push_back(static_cast<u8>(value >> 8));
}

void put32(std::vector<u8>& out, const u32 value) {
  out.push_back(static_cast<u8>(value));
  out.push_back(static_cast<u8>(value >> 8));
  out.push_back(static_cast<u8>(value >> 16));
  out.push_back(static_cast<u8>(value >> 24));
}

void put64(std::vector<u8>& out, const u64 value) {
  for (u32 at = 0; at < 8; ++at) {
    out.push_back(static_cast<u8>(value >> (at * 8)));
  }
}

// A string table whose byte 0 is the empty name every ELF table needs.
struct StrTab {
  std::vector<u8> bytes{0};

  u32 add(const std::string_view name) {
    const u32 at = static_cast<u32>(bytes.size());
    bytes.insert(bytes.end(), name.begin(), name.end());
    bytes.push_back(0);
    return at;
  }
};

u64 align_up(const u64 value, const u64 align) {
  DCHECK(align != 0);
  return (value + align - 1) / align * align;
}

u32 flags_of(const Section& section) {
  u32 flags = 0;
  if (section.write) {
    flags |= SHF_WRITE;
  }
  if (section.alloc) {
    flags |= SHF_ALLOC;
  }
  if (section.exec) {
    flags |= SHF_EXECINSTR;
  }
  return flags;
}

u32 reloc_type(const RelocKind kind, const Machine machine) {
  if (machine == Machine::Riscv64) {
    switch (kind) {
      case RelocKind::Abs64: return R_RISCV_64;
      case RelocKind::Pc32: return R_RISCV_CALL;
      case RelocKind::Plt32: return R_RISCV_CALL_PLT;
    }
    return R_RISCV_CALL;
  }
  switch (kind) {
    case RelocKind::Abs64: return R_X86_64_64;
    case RelocKind::Pc32: return R_X86_64_PC32;
    case RelocKind::Plt32: return R_X86_64_PLT32;
  }
  return R_X86_64_PC32;
}

u8 symbol_info(const Symbol& symbol) {
  const u8 bind = symbol.bind == SymbolBind::Global ? 1 : 0;
  const u8 type = static_cast<u8>(symbol.type);
  return static_cast<u8>(bind << 4 | type);
}

// The final symbol table: the null entry, one section symbol per section
// a relocation touches, then the object's own locals and globals. The
// index each name lands at is what the relocation table reads.
struct SymbolTable {
  std::vector<u8> bytes;
  StrTab names;
  u32 count = 0;
  u32 first_global = 0;

  u32 add(const u32 name,
          const u8 info,
          const u16 shndx,
          const u64 value,
          const u64 size) {
    put32(bytes, name);
    bytes.push_back(info);
    bytes.push_back(0);
    put16(bytes, shndx);
    put64(bytes, value);
    put64(bytes, size);
    return count++;
  }
};

}  // namespace

std::vector<u8> write_elf64(const Object& object, const Machine machine) {
  // The ELF section table opens with a null section, so a caller's
  // section `i` is header `i + 1` and every symbol's `st_shndx` says so.
  const u32 section_count = static_cast<u32>(object.sections.size());

  // Which sections carry relocations, and the relocation lists per one.
  std::vector<bool> relocated(section_count, false);
  for (const Relocation& reloc : object.relocations) {
    DCHECK(reloc.section < section_count);
    relocated[reloc.section] = true;
  }

  // Symbol resolution. A relocation names a symbol the emitter added; a
  // name nothing carries would write an object the linker refuses, so
  // the table is built once here and every lookup reads it.
  SymbolTable table;
  table.add(0, 0, 0, 0, 0);  // The null symbol every table opens with.
  std::vector<u32> section_symbol_of(section_count, 0);
  for (u32 at = 0; at < section_count; ++at) {
    if (!relocated[at]) {
      continue;
    }
    section_symbol_of[at] =
        table.add(table.names.add(object.sections[at].name), /*info=*/3,
                  /*shndx=*/at + 1, 0, 0);
  }
  std::vector<u32> symbol_index(object.symbols.size(), 0);
  // A name is resolved once per relocation; index it while the table
  // is built instead of scanning every symbol for every relocation.
  std::unordered_map<std::string_view, u32> symbol_by_name;
  symbol_by_name.reserve(object.symbols.size());
  // The table's own order: the locals first, the globals after. A
  // symbol table whose globals are not last is not one the linker
  // reads, which is what `sh_info` tells it.
  for (u32 pass = 0; pass < 2; ++pass) {
    for (u32 at = 0; at < object.symbols.size(); ++at) {
      const Symbol& symbol = object.symbols[at];
      const bool global = symbol.bind == SymbolBind::Global;
      if (global == (pass == 1)) {
        // An undefined symbol names something the link resolves; its
        // section index is zero, which is how the format spells that.
        const u16 shndx = symbol.section == Symbol::UNDEFINED
                              ? 0
                              : static_cast<u16>(symbol.section + 1);
        symbol_index[at] =
            table.add(table.names.add(symbol.name), symbol_info(symbol), shndx,
                      symbol.offset, symbol.size);
        symbol_by_name.emplace(std::string_view(symbol.name), symbol_index[at]);
      }
    }
    if (pass == 0) {
      // The boundary the locals ended at, which is what `sh_info` says.
      table.first_global = table.count;
    }
  }

  auto resolve = [&](const std::string_view name) {
    const auto found = symbol_by_name.find(name);
    if (found != symbol_by_name.end()) {
      return found->second;
    }
    CHECK_MSG(false, "relocation names a symbol the object does not carry");
    return u32{0};
  };

  // Placement. Content first, then the tables, then the header table at
  // its own 8-byte boundary.
  u64 offset = EH_SIZE;
  std::vector<u64> section_offset(section_count, 0);
  for (u32 at = 0; at < section_count; ++at) {
    const Section& section = object.sections[at];
    const u64 align = section.align == 0 ? 1 : section.align;
    offset = align_up(offset, align);
    section_offset[at] = offset;
    if (section.bits) {
      offset += section.bytes.size();
    }
  }
  const u64 note_offset = align_up(offset, 1);

  // The `.note.GNU-stack` section every Linux object carries: absent it,
  // the linker reads the object as asking for an executable stack.
  const bool needs_note = [&] {
    for (const Section& section : object.sections) {
      if (section.name == ".note.GNU-stack") {
        return false;
      }
    }
    return true;
  }();

  // Sections the writer owns, after the caller's.
  const u32 text_section = [&] {
    for (u32 at = 0; at < section_count; ++at) {
      if (object.sections[at].exec) {
        // The ELF table opens with a null section, so a caller's section
        // `i` is header `i + 1`, and that is the index the relocation
        // table names.
        return at + 1;
      }
    }
    return u32{1};
  }();
  const u32 rela_index = section_count + 1;
  const u32 symtab_index = rela_index + 1;
  const u32 strtab_index = symtab_index + 1;
  const u32 shstrtab_index = strtab_index + 1;
  const u32 header_count = shstrtab_index + 1 + (needs_note ? 1 : 0);

  u64 rela_offset = align_up(note_offset, 8);
  const u64 rela_size = object.relocations.size() * RELA_SIZE;
  const u64 symtab_offset = align_up(rela_offset + rela_size, 8);
  const u64 symtab_size = table.bytes.size();
  u64 strtab_offset = align_up(symtab_offset + symtab_size, 1);
  const u64 strtab_size = table.names.bytes.size();
  strtab_offset = align_up(strtab_offset, 1);
  StrTab shstrtab;
  const u32 null_name = shstrtab.add("");
  std::vector<u32> shstrtab_name_of(section_count, 0);
  for (u32 at = 0; at < section_count; ++at) {
    shstrtab_name_of[at] = shstrtab.add(object.sections[at].name);
  }
  const std::string rela_section =
      object.relocations.empty()
          ? ".rela.text"
          : ".rela" + object.sections[object.relocations.front().section].name;
  const u32 rela_name = shstrtab.add(rela_section);
  const u32 symtab_name = shstrtab.add(".symtab");
  const u32 strtab_name = shstrtab.add(".strtab");
  const u32 shstrtab_name = shstrtab.add(".shstrtab");
  const u32 note_name = shstrtab.add(".note.GNU-stack");
  const u64 shstrtab_offset = align_up(strtab_offset + strtab_size, 1);
  const u64 shstrtab_size = shstrtab.bytes.size();
  const u64 shoff = align_up(shstrtab_offset + shstrtab_size, 8);

  std::vector<u8> out;
  out.reserve(static_cast<usize>(shoff + header_count * SH_SIZE));

  // ELF header.
  const u8 ident[16] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0,
                        0,    0,   0,   0,   0, 0, 0, 0};
  out.insert(out.end(), ident, ident + 16);
  put16(out, ET_REL);
  put16(out, static_cast<u16>(machine));
  put32(out, 1);
  put64(out, 0);
  put64(out, 0);
  put64(out, shoff);
  put32(out, 0);
  put16(out, EH_SIZE);
  put16(out, 0);
  put16(out, 0);
  put16(out, SH_SIZE);
  put16(out, static_cast<u16>(header_count));
  put16(out, static_cast<u16>(shstrtab_index));

  // Grows the output to `at`, padding with zeroes; a section the file
  // reserves without bytes lands in the gap.
  const auto pad_to = [&](const u64 at, const u64 align) {
    const u64 rounded = align_up(at, align);
    if (rounded > out.size()) {
      out.resize(static_cast<usize>(rounded), 0);
    }
  };

  for (u32 at = 0; at < section_count; ++at) {
    const Section& section = object.sections[at];
    const u64 align = section.align == 0 ? 1 : section.align;
    pad_to(section_offset[at], align);
    DCHECK(out.size() == section_offset[at]);
    if (section.bits) {
      out.insert(out.end(), section.bytes.begin(), section.bytes.end());
    }
  }

  // Relocation table for the code section.
  if (!object.relocations.empty()) {
    pad_to(rela_offset, 8);
    DCHECK(out.size() == rela_offset);
    for (const Relocation& reloc : object.relocations) {
      put64(out, reloc.offset);
      const u64 info = static_cast<u64>(resolve(reloc.symbol)) << 32 |
                       reloc_type(reloc.kind, machine);
      put64(out, info);
      put64(out, static_cast<u64>(reloc.addend));
    }
  }

  pad_to(symtab_offset, 8);
  DCHECK(out.size() == symtab_offset);
  out.insert(out.end(), table.bytes.begin(), table.bytes.end());

  pad_to(strtab_offset, 1);
  DCHECK(out.size() == strtab_offset);
  out.insert(out.end(), table.names.bytes.begin(), table.names.bytes.end());

  pad_to(shstrtab_offset, 1);
  DCHECK(out.size() == shstrtab_offset);
  out.insert(out.end(), shstrtab.bytes.begin(), shstrtab.bytes.end());

  pad_to(shoff, 8);
  DCHECK(out.size() == shoff);

  const auto header = [&](const u32 name, const u32 type, const u64 flags,
                          const u64 addr, const u64 at, const u64 size,
                          const u32 link, const u32 info, const u64 align,
                          const u64 entsize) {
    put32(out, name);
    put32(out, type);
    put64(out, flags);
    put64(out, addr);
    put64(out, at);
    put64(out, size);
    put32(out, link);
    put32(out, info);
    put64(out, align);
    put64(out, entsize);
  };

  header(null_name, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  for (u32 at = 0; at < section_count; ++at) {
    const Section& section = object.sections[at];
    header(shstrtab_name_of[at], section.bits ? SHT_PROGBITS : SHT_NOBITS,
           flags_of(section), 0, section_offset[at], section.bytes.size(), 0, 0,
           section.align == 0 ? 1 : section.align, 0);
  }
  if (!object.relocations.empty()) {
    header(rela_name, SHT_RELA, 0, 0, rela_offset, rela_size, symtab_index,
           text_section, 8, RELA_SIZE);
  } else {
    header(rela_name, SHT_RELA, 0, 0, 0, 0, symtab_index, text_section, 8,
           RELA_SIZE);
  }
  header(symtab_name, SHT_SYMTAB, 0, 0, symtab_offset, symtab_size,
         strtab_index, table.first_global, 8, SYM_SIZE);
  header(strtab_name, SHT_STRTAB, 0, 0, strtab_offset, strtab_size, 0, 0, 1, 0);
  header(shstrtab_name, SHT_STRTAB, 0, 0, shstrtab_offset, shstrtab_size, 0, 0,
         1, 0);
  if (needs_note) {
    header(note_name, SHT_PROGBITS, 0, 0, note_offset, 0, 0, 0, 1, 0);
  }

  return out;
}

std::vector<u8> write_elf64_x86_64(const Object& object) {
  return write_elf64(object, Machine::X86_64);
}

}  // namespace codegen::object
