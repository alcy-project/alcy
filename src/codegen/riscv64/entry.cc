// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/riscv64/entry.h"

#include <string_view>
#include <vector>

#include "codegen/riscv64/isa.h"
#include "codegen/riscv64/materialize.h"
#include "codegen/riscv64/minst.h"
#include "codegen/riscv64/rodata.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/storage.h"
#include "ir/type.h"

namespace codegen::riscv64 {

EntryBody build_entry(const ir::Function& function,
                      const ir::Storage& storage,
                      const ir::PointerWidth width,
                      RoData& rodata) {
  EntryBody entry;
  const ir::TypeIdx result_type = function.meta.return_type;
  const ir::TypeTag result_tag = storage.types()[result_type.idx].tag;
  entry.returns_enum = result_tag == ir::TypeTag::Enum;
  if (entry.returns_enum) {
    const u64 raw = storage.layout_of(result_type, width).size;
    entry.sret_bytes = static_cast<i64>((raw + 15) & ~15ull);
  }
  constexpr std::string_view kErr = "main returned Err\n";
  if (entry.returns_enum) {
    entry.err_index = rodata.intern(kErr);
  }

  std::vector<MInst>& wrapper = entry.insts;
  // Read the entry stack before any reservation moves sp.
  wrapper.push_back(make_inst(MOp::Ld, 8, Operand{Reg::X21},
                              Operand{Mem{.base = RegRef{SP}, .disp = 0}}));
  wrapper.push_back(
      make_inst(MOp::Addi, 8, Operand{Reg::X4}, Operand{SP}, Operand{Imm{8}}));
  if (entry.sret_bytes > 0) {
    if (fits_i12(entry.sret_bytes)) {
      wrapper.push_back(make_inst(MOp::Addi, 8, Operand{SP}, Operand{SP},
                                  Operand{Imm{-entry.sret_bytes}}));
    } else {
      emit_materialize(wrapper, T0, entry.sret_bytes);
      wrapper.push_back(
          make_inst(MOp::Sub, 8, Operand{SP}, Operand{SP}, Operand{T0}));
    }
    wrapper.push_back(
        make_inst(MOp::Addi, 8, Operand{A0}, Operand{SP}, Operand{Imm{0}}));
  }
  // Terminate the backtrace frame chain at the process entry.
  wrapper.push_back(
      make_inst(MOp::Addi, 8, Operand{S1}, Operand{ZERO}, Operand{Imm{0}}));
  entry.call_at = static_cast<u32>(wrapper.size());
  wrapper.push_back(make_inst(MOp::Jal, 8, Operand{RA}, Operand{Imm{0}}));
  if (entry.returns_enum) {
    const u8 width = ir::enum_tag_width(storage.state(), result_type);
    const MOp load = width == 1   ? MOp::Lbu
                     : width == 2 ? MOp::Lhu
                     : width == 4 ? MOp::Lwu
                                  : MOp::Ld;
    wrapper.push_back(make_inst(load, width, Operand{T1},
                                Operand{Mem{.base = RegRef{SP}, .disp = 0}}));
    const u32 ok = entry.labels++;
    const u32 exit_label = entry.labels++;
    MInst branch =
        make_inst(MOp::Beq, 8, Operand{T1}, Operand{ZERO}, Operand{Label{ok}});
    branch.cond = Cond::Eq;
    wrapper.push_back(branch);
    // Err: print the reason to stderr, then exit with status one.
    entry.message_at = static_cast<u32>(wrapper.size());
    wrapper.push_back(make_inst(MOp::Auipc, 8, Operand{A1}, Operand{Imm{0}}));
    wrapper.push_back(
        make_inst(MOp::Addi, 8, Operand{A1}, Operand{A1}, Operand{Imm{0}}));
    wrapper.push_back(
        make_inst(MOp::Addi, 8, Operand{A7}, Operand{ZERO}, Operand{Imm{64}}));
    wrapper.push_back(
        make_inst(MOp::Addi, 8, Operand{A0}, Operand{ZERO}, Operand{Imm{2}}));
    wrapper.push_back(make_inst(MOp::Addi, 8, Operand{A2}, Operand{ZERO},
                                Operand{Imm{static_cast<i64>(kErr.size())}}));
    wrapper.push_back(make_inst(MOp::Ecall, 8));
    wrapper.push_back(
        make_inst(MOp::Addi, 8, Operand{A0}, Operand{ZERO}, Operand{Imm{1}}));
    wrapper.push_back(
        make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{exit_label}}));
    MInst at_ok = make_inst(MOp::Nop, 8);
    at_ok.label = ok;
    wrapper.push_back(at_ok);
    wrapper.push_back(
        make_inst(MOp::Addi, 8, Operand{A0}, Operand{ZERO}, Operand{Imm{0}}));
    MInst at_exit = make_inst(MOp::Nop, 8);
    at_exit.label = exit_label;
    wrapper.push_back(at_exit);
  }
  wrapper.push_back(
      make_inst(MOp::Addi, 8, Operand{A7}, Operand{ZERO}, Operand{Imm{93}}));
  wrapper.push_back(make_inst(MOp::Ecall, 8));
  return entry;
}

}  // namespace codegen::riscv64
