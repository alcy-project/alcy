// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/printer.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "codegen/object/elf64.h"
#include "codegen/x86_64/isa.h"
#include "codegen/x86_64/minst.h"
#include "fpag/base/numeric.h"

namespace codegen::x86_64 {
namespace {

constexpr const char* GPR64[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                 "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                                 "r12", "r13", "r14", "r15"};
constexpr const char* GPR32[] = {"eax",  "ecx",  "edx",  "ebx", "esp",  "ebp",
                                 "esi",  "edi",  "r8d",  "r9d", "r10d", "r11d",
                                 "r12d", "r13d", "r14d", "r15d"};
constexpr const char* GPR16[] = {"ax",   "cx",   "dx",   "bx",  "sp",   "bp",
                                 "si",   "di",   "r8w",  "r9w", "r10w", "r11w",
                                 "r12w", "r13w", "r14w", "r15w"};
constexpr const char* GPR8[] = {"al",   "cl",   "dl",   "bl",  "spl",  "bpl",
                                "sil",  "dil",  "r8b",  "r9b", "r10b", "r11b",
                                "r12b", "r13b", "r14b", "r15b"};

const char* width_suffix(const u8 width) {
  switch (width) {
    case 1: return "b";
    case 2: return "w";
    case 4: return "l";
    default: return "q";
  }
}

std::string reg_name(const Reg reg, const u8 width) {
  const u32 index = static_cast<u32>(reg);
  if (is_xmm(reg)) {
    return "%xmm" + std::to_string(index - GPR_COUNT);
  }
  const char* const* names = GPR64;
  if (width == 1) {
    names = GPR8;
  } else if (width == 2) {
    names = GPR16;
  } else if (width == 4) {
    names = GPR32;
  }
  return std::string("%") + names[index];
}

const char* cond_suffix(const Cond cond) {
  switch (cond) {
    case Cond::O: return "o";
    case Cond::No: return "no";
    case Cond::E: return "e";
    case Cond::Ne: return "ne";
    case Cond::L: return "l";
    case Cond::Le: return "le";
    case Cond::G: return "g";
    case Cond::Ge: return "ge";
    case Cond::B: return "b";
    case Cond::Be: return "be";
    case Cond::A: return "a";
    case Cond::Ae: return "ae";
    case Cond::S: return "s";
    case Cond::Ns: return "ns";
    case Cond::P: return "p";
    case Cond::Np: return "np";
  }
  return "";
}

std::string memory_text(const Mem& mem) {
  std::string out;
  if (mem.disp != 0) {
    out += std::to_string(mem.disp);
  }
  const auto base_reg = [&]() -> std::optional<Reg> {
    if (!mem.base.has_value()) {
      return std::nullopt;
    }
    if (const Reg* reg = std::get_if<Reg>(&*mem.base); reg != nullptr) {
      return *reg;
    }
    return std::nullopt;
  }();
  if (!base_reg.has_value()) {
    return out.empty() ? "0" : out;
  }
  out += '(' + reg_name(*base_reg, 8);
  if (mem.index.has_value()) {
    if (const Reg* index = std::get_if<Reg>(&*mem.index); index != nullptr) {
      out += ',' + reg_name(*index, 8) + "," + std::to_string(mem.scale);
    }
  }
  out += ')';
  return out;
}

// `symbol_address` is set where the encoder writes the symbol's address
// into an immediate (a mov from a SymbolRef), rather than a branch target.
std::string operand_text(const Operand& operand,
                         const Body& body,
                         const u8 width,
                         const bool symbol_address,
                         const std::string& label_prefix) {
  if (const Reg* reg = std::get_if<Reg>(&operand); reg != nullptr) {
    return reg_name(*reg, width);
  }
  if (const Imm* imm = std::get_if<Imm>(&operand); imm != nullptr) {
    return "$" + std::to_string(imm->value);
  }
  if (const Label* label = std::get_if<Label>(&operand); label != nullptr) {
    return label_prefix + ".L" + std::to_string(label->id);
  }
  if (const SymbolRef* symbol = std::get_if<SymbolRef>(&operand);
      symbol != nullptr) {
    const std::string& name = body.symbols[symbol->id];
    return symbol_address ? "$" + name : name;
  }
  if (const Mem* mem = std::get_if<Mem>(&operand); mem != nullptr) {
    return memory_text(*mem);
  }
  return "<unresolved>";
}

bool is_symbol(const Operand& operand) {
  return std::holds_alternative<SymbolRef>(operand);
}

std::string instruction_text(const MInst& inst,
                             const Body& body,
                             const std::string& label_prefix) {
  const u8 w = inst.width;
  const std::string suf = width_suffix(w);
  const bool symbol_src = inst.count > 1 && is_symbol(inst.at(1));
  const auto op = [&](const u32 index, const u8 width) {
    return operand_text(inst.at(index), body, width, false, label_prefix);
  };
  const auto op_addr = [&](const u32 index, const u8 width) {
    return operand_text(inst.at(index), body, width, true, label_prefix);
  };

  switch (inst.op) {
    case MOp::Mov:
      return std::string(symbol_src && w == 8 ? "movabsq" : ("mov" + suf)) +
             " " + (symbol_src ? op_addr(1, w) : op(1, w)) + ", " + op(0, w);
    case MOp::Movzx:
    case MOp::Movsx: {
      if (w == 4) {
        // A 32-bit source has no two-byte extend form: a plain 32-bit
        // move zero-extends, and movsxd is spelled movslq.
        return std::string(inst.op == MOp::Movzx ? "movl " : "movslq ") +
               op(1, 4) + ", " + op(0, inst.op == MOp::Movzx ? 4 : 8);
      }
      const char* kind = inst.op == MOp::Movzx ? "movz" : "movs";
      return std::string(kind) + (w == 1 ? "bq " : "wq ") + op(1, w) + ", " +
             op(0, 8);
    }
    case MOp::Lea: return "leaq " + op(1, 8) + ", " + op(0, 8);
    case MOp::Add: return "add" + suf + " " + op(1, w) + ", " + op(0, w);
    case MOp::Sub: return "sub" + suf + " " + op(1, w) + ", " + op(0, w);
    case MOp::And: return "and" + suf + " " + op(1, w) + ", " + op(0, w);
    case MOp::Or: return "or" + suf + " " + op(1, w) + ", " + op(0, w);
    case MOp::Xor: return "xor" + suf + " " + op(1, w) + ", " + op(0, w);
    case MOp::Imul: return "imul" + suf + " " + op(1, w) + ", " + op(0, w);
    case MOp::Umul: return "mul" + suf + " " + op(0, w);
    case MOp::Idiv: return "idiv" + suf + " " + op(0, w);
    case MOp::Udiv: return "div" + suf + " " + op(0, w);
    case MOp::Shl:
    case MOp::Sar:
    case MOp::Shr: {
      // The encoder reads a register count from cl, whatever the
      // instruction's operand width is.
      const std::string count = std::holds_alternative<Reg>(inst.at(1))
                                    ? reg_name(Reg::Rcx, 1)
                                    : op(1, w);
      const char* mnemonic = inst.op == MOp::Shl   ? "shl"
                             : inst.op == MOp::Sar ? "sar"
                                                   : "shr";
      return std::string(mnemonic) + suf + " " + count + ", " + op(0, w);
    }
    case MOp::Not: return "not" + suf + " " + op(0, w);
    case MOp::Neg: return "neg" + suf + " " + op(0, w);
    case MOp::Cmp: return "cmp" + suf + " " + op(1, w) + ", " + op(0, w);
    case MOp::Test: return "test" + suf + " " + op(1, w) + ", " + op(0, w);
    case MOp::Setcc:
      return std::string("set") + cond_suffix(inst.cond) + " " + op(0, 1);
    case MOp::Jmp: return "jmp " + op(0, 8);
    case MOp::Jcc:
      return std::string("j") + cond_suffix(inst.cond) + " " + op(0, 8);
    case MOp::Call:
      return std::string("call ") +
             (is_symbol(inst.at(0)) ? op(0, 8) : "*" + op(0, 8));
    case MOp::CallReg: return "call *" + op(0, 8);
    case MOp::Ret: return "ret";
    case MOp::Syscall: return "syscall";
    case MOp::Nop: return "nop";
    case MOp::Push: return "pushq " + op(0, 8);
    case MOp::Pop: return "popq " + op(0, 8);
    case MOp::Cqo: return "cqo";
    case MOp::RepMovsb: return "rep movsb";
    case MOp::RepStosb: return "rep stosb";
    case MOp::Movss: return "movss " + op(1, w) + ", " + op(0, w);
    case MOp::Movsd: return "movsd " + op(1, w) + ", " + op(0, w);
    case MOp::Movdqu: return "movdqu " + op(1, 16) + ", " + op(0, 16);
    case MOp::Fadd:
    case MOp::Fsub:
    case MOp::Fmul:
    case MOp::Fdiv: {
      const char* mnemonic = inst.op == MOp::Fadd   ? "add"
                             : inst.op == MOp::Fsub ? "sub"
                             : inst.op == MOp::Fmul ? "mul"
                                                    : "div";
      return std::string(mnemonic) + (w == 4 ? "ss " : "sd ") + op(1, w) +
             ", " + op(0, w);
    }
    case MOp::Fneg: return "xorpd " + op(1, 16) + ", " + op(0, 16);
    case MOp::Fcom:
      return std::string(w == 4 ? "comiss " : "comisd ") + op(1, w) + ", " +
             op(0, w);
    case MOp::CvtI2F: {
      const i64 width = inst.count > 2 ? std::get<Imm>(inst.at(2)).value : 8;
      return std::string(width == 8 ? "cvtsi2sd " : "cvtsi2ss ") + op(1, 8) +
             ", " + op(0, w);
    }
    case MOp::CvtF2I: {
      const i64 dest = inst.count > 2 ? std::get<Imm>(inst.at(2)).value : 4;
      return std::string(w == 4 ? "cvttss2si " : "cvttsd2si ") + op(1, w) +
             ", " + op(0, static_cast<u8>(dest));
    }
    case MOp::CvtF2F:
      return std::string(w == 4 ? "cvtss2sd " : "cvtsd2ss ") + op(1, w) + ", " +
             op(0, w);
    case MOp::Ud2: return "ud2";
  }
  return "<unknown>";
}

void emit_bytes(std::string& out, const std::vector<u8>& bytes) {
  constexpr usize PER_LINE = 16;
  for (usize at = 0; at < bytes.size(); at += PER_LINE) {
    out += "\t.byte ";
    const usize end = std::min(at + PER_LINE, bytes.size());
    for (usize i = at; i < end; ++i) {
      if (i != at) {
        out += ", ";
      }
      out += "0x";
      const u8 value = bytes[i];
      out += "0123456789abcdef"[value >> 4];
      out += "0123456789abcdef"[value & 0xf];
    }
    out += '\n';
  }
}

void emit_data_section(std::string& out,
                       const object::Object& object,
                       const u32 section_index) {
  const object::Section& section = object.sections[section_index];
  out += "\t.section " + section.name + ",\"" + (section.write ? "aw" : "a") +
         "\",@progbits\n";
  out += "\t.balign " + std::to_string(section.align) + "\n";
  std::vector<const object::Symbol*> symbols;
  for (const object::Symbol& symbol : object.symbols) {
    if (symbol.section == section_index) {
      symbols.push_back(&symbol);
    }
  }
  std::sort(symbols.begin(), symbols.end(),
            [](const object::Symbol* a, const object::Symbol* b) {
              return a->offset < b->offset;
            });
  usize at = 0;
  for (const object::Symbol* symbol : symbols) {
    while (at < symbol->offset && at < section.bytes.size()) {
      const usize end = std::min(symbol->offset, section.bytes.size());
      std::vector<u8> gap(section.bytes.begin() + at,
                          section.bytes.begin() + end);
      emit_bytes(out, gap);
      at = end;
    }
    if (symbol->bind == object::SymbolBind::Global) {
      out += "\t.globl " + symbol->name + "\n";
    }
    out += "\t.type " + symbol->name + ",@object\n";
    out += symbol->name + ":\n";
  }
  std::vector<u8> rest(section.bytes.begin() + at, section.bytes.end());
  emit_bytes(out, rest);
}

}  // namespace

std::string print_assembly(const object::Object& object,
                           std::span<const PrintedBody> bodies,
                           const Syntax syntax) {
  if (syntax != Syntax::Att) {
    return std::string("; Intel syntax is not available yet\n");
  }
  std::string out;
  out += "\t.text\n";
  out += "\t.section .text,\"ax\",@progbits\n";
  out += "\t.balign 16\n";

  for (const object::Symbol& symbol : object.symbols) {
    if (symbol.section != 0) {
      continue;
    }
    if (symbol.bind == object::SymbolBind::Global) {
      out += "\t.globl " + symbol.name + "\n";
    }
    if (symbol.type == object::SymbolType::Func) {
      out += "\t.type " + symbol.name + ",@function\n";
    }
  }

  // The text section carries both instructions and data: the backtrace
  // tables follow the bodies. Walk it by offset so the data symbols get
  // labels too, and print a body's instructions where its bytes begin.
  const std::vector<u8>& text_bytes = object.sections[0].bytes;
  std::vector<const PrintedBody*> ordered;
  ordered.reserve(bodies.size());
  for (const PrintedBody& printed : bodies) {
    ordered.push_back(&printed);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const PrintedBody* a, const PrintedBody* b) {
              return a->offset < b->offset;
            });
  usize body_at = 0;
  usize at = 0;
  while (at < text_bytes.size()) {
    usize next = text_bytes.size();
    for (const object::Symbol& symbol : object.symbols) {
      if (symbol.section == 0 && symbol.offset > at && symbol.offset < next) {
        next = static_cast<usize>(symbol.offset);
      }
    }
    while (body_at < ordered.size() && ordered[body_at]->offset < at) {
      ++body_at;
    }
    if (body_at < ordered.size() && ordered[body_at]->offset == at) {
      next = std::min(next, static_cast<usize>(ordered[body_at]->offset +
                                               ordered[body_at]->size));
    }
    // A label at this offset belongs to a body only when the body's
    // instructions are what follow; data symbols get a plain label.
    bool is_body = body_at < ordered.size() && ordered[body_at]->offset == at;
    for (const object::Symbol& symbol : object.symbols) {
      if (symbol.section != 0 || symbol.offset != at) {
        continue;
      }
      if (symbol.type == object::SymbolType::Func && is_body) {
        continue;
      }
      out += symbol.name + ":\n";
    }
    if (is_body) {
      const PrintedBody& printed = *ordered[body_at];
      const std::string label_prefix = printed.symbol + ".";
      out += printed.symbol + ":\n";
      for (const std::string& alias : printed.aliases) {
        out += alias + ":\n";
      }
      for (const MInst& inst : printed.body.insts) {
        if (inst.label != MInst::NO_LABEL) {
          out += label_prefix + ".L" + std::to_string(inst.label) + ":\n";
        }
        out += "\t" + instruction_text(inst, printed.body, label_prefix) + "\n";
      }
      out += "\t.size " + printed.symbol + ", .-" + printed.symbol + "\n";
      at = next;
      ++body_at;
      continue;
    }
    std::vector<u8> chunk(text_bytes.begin() + at, text_bytes.begin() + next);
    emit_bytes(out, chunk);
    at = next;
  }

  for (u32 section = 1; section < object.sections.size(); ++section) {
    emit_data_section(out, object, section);
  }
  return out;
}

}  // namespace codegen::x86_64
