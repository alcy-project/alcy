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

struct AluName {
  const char* reg = nullptr;
  const char* imm = nullptr;
};

// The two spellings of an integer ALU op. A null `imm` means the encoder
// has no immediate form for it, so the register form is always used.
constexpr AluName alu_name(const MOp op) {
  switch (op) {
    case MOp::Add:
    case MOp::Addi: return {"add", "addi"};
    case MOp::Sub: return {"sub", "addi"};
    case MOp::Sll:
    case MOp::Slli: return {"sll", "slli"};
    case MOp::Slt:
    case MOp::Slti: return {"slt", "slti"};
    case MOp::Sltu:
    case MOp::Sltiu: return {"sltu", "sltiu"};
    case MOp::Xor:
    case MOp::Xori: return {"xor", "xori"};
    case MOp::Srl:
    case MOp::Srli: return {"srl", "srli"};
    case MOp::Sra:
    case MOp::Srai: return {"sra", "srai"};
    case MOp::Or:
    case MOp::Ori: return {"or", "ori"};
    case MOp::And:
    case MOp::Andi: return {"and", "andi"};
    case MOp::Mul: return {"mul", nullptr};
    case MOp::Mulh: return {"mulh", nullptr};
    case MOp::Mulhu: return {"mulhu", nullptr};
    case MOp::Div: return {"div", nullptr};
    case MOp::Divu: return {"divu", nullptr};
    case MOp::Rem: return {"rem", nullptr};
    case MOp::Remu: return {"remu", nullptr};
    default: return {};
  }
}

// The encoder's `reg()` maps a non-register operand to x0; print the same
// register so the source assembles to the same instruction.
std::string reg_or_zero(const Operand& op) {
  if (const Reg* reg = std::get_if<Reg>(&op)) {
    return reg_name(*reg);
  }
  return "zero";
}

std::string reg_ref_name(const RegRef& ref) {
  if (const Reg* reg = std::get_if<Reg>(&ref)) {
    return reg_name(*reg);
  }
  return "<vreg>";
}

// Mangled names may carry ':' which GNU as reads as a label suffix.
std::string asm_name(const std::string& name) {
  std::string out = name;
  for (char& c : out) {
    if (c == ':') {
      c = '_';
    }
  }
  return out;
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
      if (const SymbolRef* symbol = std::get_if<SymbolRef>(&inst.at(1))) {
        if (symbol->id < body.symbols.size()) {
          return std::string(inst.op == MOp::Lui ? "lui " : "auipc ") + a(0) +
                 ", " +
                 std::string(inst.op == MOp::Lui ? "%hi(" : "%pcrel_hi(") +
                 asm_name(body.symbols[symbol->id]) + ")";
        }
      }
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
    case MOp::Add:
    case MOp::Sub:
    case MOp::Addi:
    case MOp::Slt:
    case MOp::Slti:
    case MOp::Sltu:
    case MOp::Sltiu:
    case MOp::Xor:
    case MOp::Xori:
    case MOp::Or:
    case MOp::Ori:
    case MOp::And:
    case MOp::Andi:
    case MOp::Sll:
    case MOp::Slli:
    case MOp::Srl:
    case MOp::Srli:
    case MOp::Sra:
    case MOp::Srai:
    case MOp::Mul:
    case MOp::Mulh:
    case MOp::Mulhu:
    case MOp::Div:
    case MOp::Divu:
    case MOp::Rem:
    case MOp::Remu: {
      for (const Operand& op : inst.ops) {
        if (const SymbolRef* symbol = std::get_if<SymbolRef>(&op)) {
          if (symbol->id < body.symbols.size()) {
            const char* mnemonic = alu_name(inst.op).imm != nullptr
                                       ? alu_name(inst.op).imm
                                       : "addi";
            return std::string(mnemonic) + " " + a(0) + ", " + a(1) + ", %lo(" +
                   asm_name(body.symbols[symbol->id]) + ")";
          }
        }
      }
      const AluName name = alu_name(inst.op);
      const AluOperands alu = alu_operands(inst);
      if (alu.immediate != nullptr && name.imm != nullptr) {
        const i64 value = std::get<Imm>(*alu.immediate).value;
        // There is no subi: subtraction adds the negated immediate.
        const i64 encoded = inst.op == MOp::Sub ? -value : value;
        return std::string(name.imm) + " " + a(0) + ", " +
               reg_or_zero(*alu.rs1) + ", " + std::to_string(encoded);
      }
      return std::string(name.reg) + " " + a(0) + ", " + reg_or_zero(*alu.rs1) +
             ", " + reg_or_zero(*alu.rs2);
    }
    case MOp::Fadd:
    case MOp::Fsub:
    case MOp::Fmul:
    case MOp::Fdiv:
    case MOp::Feq:
    case MOp::Flt:
    case MOp::Fle: {
      const bool compare =
          inst.op == MOp::Feq || inst.op == MOp::Flt || inst.op == MOp::Fle;
      const char* mnemonic = inst.op == MOp::Fadd   ? "fadd"
                             : inst.op == MOp::Fsub ? "fsub"
                             : inst.op == MOp::Fmul ? "fmul"
                             : inst.op == MOp::Fdiv ? "fdiv"
                             : inst.op == MOp::Feq  ? "feq"
                             : inst.op == MOp::Flt  ? "flt"
                                                    : "fle";
      std::string text = std::string(mnemonic);
      text += inst.width == 4 ? ".s" : ".d";
      // The encoder selects RNE (funct3=0). GNU as defaults to the
      // dynamic mode, so ask for RNE explicitly to match the object.
      return text + " " + a(0) + ", " + a(1) + ", " + a(2) +
             (compare ? "" : ", rne");
    }
    case MOp::Fmov:
      // The encoder uses the same register for both source operands;
      // there is no third operand to print.
      return std::string(inst.width == 4 ? "fsgnj.s " : "fsgnj.d ") + a(0) +
             ", " + a(1) + ", " + a(1);
    case MOp::Fcvt: {
      const FcvtMode mode =
          inst.count > 2 && std::holds_alternative<FcvtMode>(inst.at(2))
              ? std::get<FcvtMode>(inst.at(2))
              : FcvtMode::F64ToI64;
      const bool to_int =
          mode == FcvtMode::F64ToI64 || mode == FcvtMode::F32ToI64 ||
          mode == FcvtMode::F64ToIU64 || mode == FcvtMode::F32ToIU64;
      return std::string(fcvt_mnemonic(mode)) + " " + a(0) + ", " + a(1) +
             (to_int ? ", rtz" : ", rne");
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
  out += "\t.option norelax\n";
  out += "\t.text\n";
  out += "\t.section .text,\"ax\",@progbits\n";
  out += "\t.balign 16\n";

  for (const object::Symbol& symbol : object.symbols) {
    if (symbol.section != 0) {
      continue;
    }
    if (symbol.bind == object::SymbolBind::Global) {
      out += "\t.globl " + asm_name(symbol.name) + "\n";
    }
    if (symbol.type == object::SymbolType::Func) {
      out += "\t.type " + asm_name(symbol.name) + ",@function\n";
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
        label(asm_name(symbol.name));
      }
    }
    if (is_body) {
      const PrintedBody& printed = *ordered[body_at];
      const std::string prefix = asm_name(printed.symbol) + ".";
      label(asm_name(printed.symbol));
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
      out += "\t.size " + asm_name(printed.symbol) + ", .-" +
             asm_name(printed.symbol) + "\n";
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
