// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/riscv64/printer.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

#include "codegen/object/elf64.h"
#include "codegen/riscv64/isa.h"
#include "codegen/riscv64/minst.h"
#include "fpag/base/numeric.h"

namespace codegen::riscv64 {
namespace {

constexpr const char* GPR[] = {
    "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
    "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
    "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

const char* reg_name(const Reg reg) {
  if (is_fpr(reg)) {
    static std::string names[32];
    static bool initialized = false;
    if (!initialized) {
      for (u32 i = 0; i < 32; ++i) {
        names[i] = "f" + std::to_string(i);
      }
      initialized = true;
    }
    return names[reg_code(reg)].c_str();
  }
  return GPR[reg_code(reg)];
}

const char* branch_mnemonic(const Cond cond) {
  switch (cond) {
    case Cond::Eq: return "beq";
    case Cond::Ne: return "bne";
    case Cond::Lt: return "blt";
    case Cond::Ge: return "bge";
    case Cond::Ltu: return "bltu";
    case Cond::Geu: return "bgeu";
  }
  return "beq";
}

const char* load_mnemonic(const MOp op) {
  switch (op) {
    case MOp::Lb: return "lb";
    case MOp::Lbu: return "lbu";
    case MOp::Lh: return "lh";
    case MOp::Lhu: return "lhu";
    case MOp::Lw: return "lw";
    case MOp::Lwu: return "lwu";
    case MOp::Ld: return "ld";
    case MOp::Flw: return "flw";
    case MOp::Fld: return "fld";
    default: return "ld";
  }
}

const char* store_mnemonic(const MOp op) {
  switch (op) {
    case MOp::Sb: return "sb";
    case MOp::Sh: return "sh";
    case MOp::Sw: return "sw";
    case MOp::Sd: return "sd";
    case MOp::Fsw: return "fsw";
    case MOp::Fsd: return "fsd";
    default: return "sd";
  }
}

const char* fcvt_mnemonic(const FcvtMode mode) {
  switch (mode) {
    case FcvtMode::F64ToI64: return "fcvt.l.d";
    case FcvtMode::F32ToI64: return "fcvt.l.s";
    case FcvtMode::I64ToF64: return "fcvt.d.l";
    case FcvtMode::I32ToF64: return "fcvt.d.w";
    case FcvtMode::F64ToIU64: return "fcvt.lu.d";
    case FcvtMode::F32ToIU64: return "fcvt.lu.s";
    case FcvtMode::F64ToF32: return "fcvt.s.d";
    case FcvtMode::F32ToF64: return "fcvt.d.s";
    case FcvtMode::IU64ToF64: return "fcvt.d.lu";
    case FcvtMode::IU32ToF64: return "fcvt.d.wu";
    case FcvtMode::IU64ToF32: return "fcvt.s.lu";
    case FcvtMode::IU32ToF32: return "fcvt.s.wu";
    case FcvtMode::I64ToF32: return "fcvt.s.l";
    case FcvtMode::I32ToF32: return "fcvt.s.w";
  }
  return "fcvt.l.d";
}

std::string reg_ref_name(const RegRef& ref) {
  if (const Reg* reg = std::get_if<Reg>(&ref)) {
    return reg_name(*reg);
  }
  return "<vreg>";
}

// A data symbol is materialized by an auipc/addi pair; print the pair in
// the pseudo form that names the symbol directly.
std::string symbol_name(const SymbolRef symbol, const Body& body) {
  if (symbol.id < body.symbols.size()) {
    return body.symbols[symbol.id];
  }
  return "alcy.str." + std::to_string(symbol.id);
}

std::string operand(const Operand& op,
                    const Body& body,
                    const std::string& label_prefix) {
  if (const Reg* reg = std::get_if<Reg>(&op)) {
    return reg_name(*reg);
  }
  if (const Imm* imm = std::get_if<Imm>(&op)) {
    return std::to_string(imm->value);
  }
  if (const Label* label = std::get_if<Label>(&op)) {
    return label_prefix + ".L" + std::to_string(label->id);
  }
  if (const SymbolRef* symbol = std::get_if<SymbolRef>(&op)) {
    return symbol_name(*symbol, body);
  }
  if (const Mem* mem = std::get_if<Mem>(&op)) {
    std::string out = std::to_string(mem->disp) + "(";
    if (mem->base.has_value()) {
      out += reg_ref_name(*mem->base);
    }
    return out + ")";
  }
  if (const SlotRef* slot = std::get_if<SlotRef>(&op)) {
    return std::to_string(slot->offset) + "(sp)";
  }
  return "<operand>";
}

std::string instruction(const MInst& inst,
                        const Body& body,
                        const std::string& label_prefix,
                        const object::Object& object,
                        const u64 address) {
  const auto a = [&](u32 i) { return operand(inst.at(i), body, label_prefix); };
  switch (inst.op) {
    case MOp::Lui:
    case MOp::Auipc: {
      const i64 encoded = std::holds_alternative<Imm>(inst.at(1))
                              ? std::get<Imm>(inst.at(1)).value
                              : 0;
      return std::string(inst.op == MOp::Lui ? "lui " : "auipc ") + a(0) +
             ", " + std::to_string((encoded >> 12) & 0xfffff);
    }
    case MOp::Jal: {
      if (inst.count > 1 && std::holds_alternative<Label>(inst.at(1))) {
        return "j " + a(1);
      }
      // A resolved displacement is relative to this instruction; spell
      // it against `.` so the assembler computes the same offset.
      if (inst.count > 1 && std::holds_alternative<Imm>(inst.at(1))) {
        const i64 delta = std::get<Imm>(inst.at(1)).value;
        const i64 target_address = static_cast<i64>(address) + delta;
        std::string target;
        for (const object::Symbol& symbol : object.symbols) {
          if (symbol.section == 0 &&
              static_cast<i64>(symbol.offset) == target_address) {
            target = symbol.name;
            break;
          }
        }
        if (target.empty()) {
          target = delta < 0 ? ". - " + std::to_string(-delta)
                             : ". + " + std::to_string(delta);
        }
        if (inst.count > 0 && std::holds_alternative<Reg>(inst.at(0)) &&
            std::get<Reg>(inst.at(0)) == ZERO) {
          return "j " + target;
        }
        return "jal " + a(0) + ", " + target;
      }
      return "jal " + a(0) + ", " + a(1);
    }
    case MOp::Jalr: return "jalr " + a(0) + ", " + a(1) + ", " + a(2);
    case MOp::Beq:
    case MOp::Bne:
    case MOp::Blt:
    case MOp::Bge:
    case MOp::Bltu:
    case MOp::Bgeu:
      return std::string(branch_mnemonic(inst.cond)) + " " + a(0) + ", " +
             a(1) + ", " + a(2);
    case MOp::Lb:
    case MOp::Lbu:
    case MOp::Lh:
    case MOp::Lhu:
    case MOp::Lw:
    case MOp::Lwu:
    case MOp::Ld:
    case MOp::Flw:
    case MOp::Fld:
      return std::string(load_mnemonic(inst.op)) + " " + a(0) + ", " + a(1);
    case MOp::Sb:
    case MOp::Sh:
    case MOp::Sw:
    case MOp::Sd:
    case MOp::Fsw:
    case MOp::Fsd:
      return std::string(store_mnemonic(inst.op)) + " " + a(0) + ", " + a(1);
    case MOp::Addi:
    case MOp::Slti:
    case MOp::Sltiu:
    case MOp::Xori:
    case MOp::Ori:
    case MOp::Andi:
    case MOp::Slli:
    case MOp::Srli:
    case MOp::Srai: {
      const char* mnemonic = inst.op == MOp::Addi    ? "addi"
                             : inst.op == MOp::Slti  ? "slti"
                             : inst.op == MOp::Sltiu ? "sltiu"
                             : inst.op == MOp::Xori  ? "xori"
                             : inst.op == MOp::Ori   ? "ori"
                             : inst.op == MOp::Andi  ? "andi"
                             : inst.op == MOp::Slli  ? "slli"
                             : inst.op == MOp::Srli  ? "srli"
                                                     : "srai";
      return std::string(mnemonic) + " " + a(0) + ", " + a(1) + ", " + a(2);
    }
    case MOp::Add:
    case MOp::Sub: {
      // The encoder folds an immediate operand into the I-type form;
      // there is no `subi`, so subtraction adds the negated immediate.
      const Imm* imm = nullptr;
      const Operand* source = nullptr;
      if (inst.count > 1 && std::get_if<Imm>(&inst.at(1)) != nullptr) {
        imm = &std::get<Imm>(inst.at(1));
        if (inst.count > 2) {
          source = &inst.at(2);
        }
      } else if (inst.count > 2 && std::get_if<Imm>(&inst.at(2)) != nullptr) {
        imm = &std::get<Imm>(inst.at(2));
        source = &inst.at(1);
      }
      if (imm == nullptr || source == nullptr) {
        return (inst.op == MOp::Add ? "add " : "sub ") + a(0) + ", " + a(1) +
               ", " + a(2);
      }
      const i64 value = inst.op == MOp::Sub ? -imm->value : imm->value;
      return "addi " + a(0) + ", " + operand(*source, body, label_prefix) +
             ", " + std::to_string(value);
    }
    case MOp::Slt:
    case MOp::Sltu:
    case MOp::Xor:
    case MOp::Or:
    case MOp::And:
    case MOp::Sll:
    case MOp::Srl:
    case MOp::Sra:
    case MOp::Mul:
    case MOp::Mulh:
    case MOp::Mulhu:
    case MOp::Div:
    case MOp::Divu:
    case MOp::Rem:
    case MOp::Remu:
    case MOp::Fadd:
    case MOp::Fsub:
    case MOp::Fmul:
    case MOp::Fdiv:
    case MOp::Feq:
    case MOp::Flt:
    case MOp::Fle:
    case MOp::Fmov: {
      const char* mnemonic = inst.op == MOp::Add     ? "add"
                             : inst.op == MOp::Sub   ? "sub"
                             : inst.op == MOp::Slt   ? "slt"
                             : inst.op == MOp::Sltu  ? "sltu"
                             : inst.op == MOp::Xor   ? "xor"
                             : inst.op == MOp::Or    ? "or"
                             : inst.op == MOp::And   ? "and"
                             : inst.op == MOp::Sll   ? "sll"
                             : inst.op == MOp::Srl   ? "srl"
                             : inst.op == MOp::Sra   ? "sra"
                             : inst.op == MOp::Mul   ? "mul"
                             : inst.op == MOp::Mulh  ? "mulh"
                             : inst.op == MOp::Mulhu ? "mulhu"
                             : inst.op == MOp::Div   ? "div"
                             : inst.op == MOp::Divu  ? "divu"
                             : inst.op == MOp::Rem   ? "rem"
                             : inst.op == MOp::Remu  ? "remu"
                             : inst.op == MOp::Fadd  ? "fadd"
                             : inst.op == MOp::Fsub  ? "fsub"
                             : inst.op == MOp::Fmul  ? "fmul"
                             : inst.op == MOp::Fdiv  ? "fdiv"
                             : inst.op == MOp::Feq   ? "feq"
                             : inst.op == MOp::Flt   ? "flt"
                             : inst.op == MOp::Fle   ? "fle"
                                                     : "fsgnj";
      std::string text = std::string(mnemonic);
      if (inst.width == 4 && mnemonic[0] == 'f') {
        text += ".s";
      } else if (inst.width == 8 && mnemonic[0] == 'f') {
        text += ".d";
      }
      return text + " " + a(0) + ", " + a(1) + ", " + a(2);
    }
    case MOp::Fcvt: {
      const FcvtMode mode =
          inst.count > 2 && std::holds_alternative<FcvtMode>(inst.at(2))
              ? std::get<FcvtMode>(inst.at(2))
              : FcvtMode::F64ToI64;
      return std::string(fcvt_mnemonic(mode)) + " " + a(0) + ", " + a(1);
    }
    case MOp::Fneg:
      return std::string(inst.width == 4 ? "fneg.s " : "fneg.d ") + a(0) +
             ", " + a(1);
    case MOp::Ecall: return "ecall";
    case MOp::Nop:
    case MOp::Addr:
    case MOp::LoadAddr: return "nop";
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
  out += "\t.section " + section.name + ",\"a\",@progbits\n";
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
      const usize end =
          std::min(static_cast<usize>(symbol->offset), section.bytes.size());
      emit_bytes(out, std::vector<u8>(section.bytes.begin() + at,
                                      section.bytes.begin() + end));
      at = end;
    }
    if (symbol->bind == object::SymbolBind::Global) {
      out += "\t.globl " + symbol->name + "\n";
    }
    out += "\t.type " + symbol->name + ",@object\n";
    out += symbol->name + ":\n";
  }
  emit_bytes(out,
             std::vector<u8>(section.bytes.begin() + at, section.bytes.end()));
}

}  // namespace

std::string print_assembly(const object::Object& object,
                           std::span<const PrintedBody> bodies) {
  std::string out;
  // The encoder emits fixed-width RV64 instructions. Tell the assembler
  // not to relax them into two-byte compressed forms, which would shift
  // every PC-relative offset the backend resolved against the object.
  out += "\t.option norvc\n";
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

  const std::vector<u8>& text = object.sections[0].bytes;
  std::vector<const PrintedBody*> ordered;
  for (const PrintedBody& body : bodies) {
    ordered.push_back(&body);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const PrintedBody* a, const PrintedBody* b) {
              return a->offset < b->offset;
            });
  std::unordered_set<std::string> labels;
  const auto label = [&](const std::string& name) {
    if (labels.insert(name).second) {
      out += name + ":\n";
    }
  };
  usize body_at = 0;
  usize at = 0;
  while (at < text.size()) {
    usize next = text.size();
    for (const object::Symbol& symbol : object.symbols) {
      if (symbol.section == 0 && symbol.offset > at && symbol.offset < next) {
        next = static_cast<usize>(symbol.offset);
      }
    }
    while (body_at < ordered.size() && ordered[body_at]->offset < at) {
      ++body_at;
    }
    const bool is_body =
        body_at < ordered.size() && ordered[body_at]->offset == at;
    if (is_body) {
      next = std::min(next, static_cast<usize>(ordered[body_at]->offset +
                                               ordered[body_at]->size));
    }
    for (const object::Symbol& symbol : object.symbols) {
      if (symbol.section == 0 && symbol.offset == at &&
          !(symbol.type == object::SymbolType::Func && is_body)) {
        label(symbol.name);
      }
    }
    if (is_body) {
      const PrintedBody& printed = *ordered[body_at];
      const std::string prefix = printed.symbol + ".";
      label(printed.symbol);
      for (u32 inst_index = 0; inst_index < printed.body.insts.size();
           ++inst_index) {
        const MInst& inst = printed.body.insts[inst_index];
        if (inst.label != MInst::NO_LABEL) {
          out += prefix + ".L" + std::to_string(inst.label) + ":\n";
        }
        out += "\t" +
               instruction(inst, printed.body, prefix, object,
                           printed.offset + static_cast<u64>(inst_index) * 4) +
               "\n";
      }
      out += "\t.size " + printed.symbol + ", .-" + printed.symbol + "\n";
    } else {
      const usize end = std::min(next, text.size());
      emit_bytes(out, std::vector<u8>(text.begin() + at, text.begin() + end));
    }
    at = next;
  }
  for (u32 section = 1; section < object.sections.size(); ++section) {
    emit_data_section(out, object, section);
  }
  return out;
}

}  // namespace codegen::riscv64
