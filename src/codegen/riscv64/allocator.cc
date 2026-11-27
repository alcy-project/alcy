// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/riscv64/allocator.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "codegen/riscv64/frame.h"
#include "codegen/riscv64/isa.h"
#include "codegen/riscv64/minst.h"

namespace codegen::riscv64 {
namespace {

constexpr Reg INTEGER_SCRATCH[] = {T0, T1, T2, T3, Reg::X29, Reg::X30, T6};
constexpr Reg FLOAT_SCRATCH[] = {Reg::F0, Reg::F1, Reg::F2, Reg::F3,
                                 Reg::F4, Reg::F5, Reg::F6, Reg::F7};

bool writes_result(const MOp op) {
  return op_traits(op).writes_result;
}

bool is_float_op(const MOp op) {
  return op_traits(op).is_float;
}

bool needs_register_operands(const MOp op) {
  return op_traits(op).needs_register_operands;
}

}  // namespace

void allocate(Body& body, const Frame& frame) {
  std::vector<MInst> out;
  out.reserve(body.insts.size() + 8);
  const auto push = [&](MInst inst) { out.push_back(inst); };
  // A label names the start of an MInst. When an instruction expands to
  // several words, attach it to the first one so a jump cannot skip the
  // address-materialization prefix.
  u32 pending_label = MInst::NO_LABEL;
  const auto emit = [&](MInst mi) {
    if (pending_label != MInst::NO_LABEL) {
      mi.label = pending_label;
      pending_label = MInst::NO_LABEL;
    }
    push(mi);
  };
  // A displacement outside the 12-bit immediate needs its address
  // materialized into a scratch before the access.
  // Use a caller-saved scratch: X24 is callee-saved and is itself saved
  // through this helper in the prologue.
  constexpr Reg kAddr = Reg::X29;
  const auto fits = fits_i12;
  const auto address = [&](i64 disp) -> Mem {
    if (fits(disp)) {
      return Mem{.base = RegRef{Reg::X2}, .disp = static_cast<i32>(disp)};
    }
    const i64 hi = upper20(disp);
    const i64 lo = lower12(disp);
    emit(make_inst(MOp::Lui, 8, Operand{kAddr}, Operand{Imm{hi << 12}}));
    push(make_inst(MOp::Addi, 8, Operand{kAddr}, Operand{kAddr},
                   Operand{Imm{lo}}));
    push(make_inst(MOp::Add, 8, Operand{kAddr}, Operand{Reg::X2},
                   Operand{kAddr}));
    return Mem{.base = RegRef{kAddr}, .disp = 0};
  };
  const auto slot = [&](const VReg vreg) {
    return address(frame.vreg_disp[vreg.index]);
  };

  const auto load_op = [&](const VReg vreg) {
    switch (frame.vreg_size[vreg.index]) {
      case 1: return frame.vreg_signed[vreg.index] ? MOp::Lb : MOp::Lbu;
      case 2: return frame.vreg_signed[vreg.index] ? MOp::Lh : MOp::Lhu;
      case 4: return frame.vreg_signed[vreg.index] ? MOp::Lw : MOp::Lwu;
      default: return MOp::Ld;
    }
  };
  const auto store_op = [&](const VReg vreg) {
    switch (frame.vreg_size[vreg.index]) {
      case 1: return MOp::Sb;
      case 2: return MOp::Sh;
      case 4: return MOp::Sw;
      default: return MOp::Sd;
    }
  };

  // A frame larger than 2048 bytes cannot ride in a single addi's
  // 12-bit immediate; materialize it into a scratch with lui/addi.
  const bool big_frame = frame.size > 2047;
  if (big_frame) {
    const i64 hi = upper20(static_cast<i64>(frame.size));
    const i64 lo = lower12(static_cast<i64>(frame.size));
    push(make_inst(MOp::Lui, 8, Operand{T0}, Operand{Imm{hi << 12}}));
    push(make_inst(MOp::Addi, 8, Operand{T0}, Operand{T0}, Operand{Imm{lo}}));
    push(make_inst(MOp::Sub, 8, Operand{Reg::X2}, Operand{Reg::X2},
                   Operand{T0}));
  } else {
    push(make_inst(MOp::Addi, 8, Operand{Reg::X2}, Operand{Reg::X2},
                   Operand{Imm{-static_cast<i64>(frame.size)}}));
  }
  for (u32 i = 0; i < std::size(SAVED_REGS); ++i) {
    push(make_inst(MOp::Sd, 8, Operand{SAVED_REGS[i]},
                   address(save_disp(frame.size, i))));
  }
  const i64 fp_disp = save_disp(frame.size, 1);
  if (fits_i12(fp_disp)) {
    push(make_inst(MOp::Addi, 8, Operand{S1}, Operand{Reg::X2},
                   Operand{Imm{fp_disp}}));
  } else {
    const i64 hi = upper20(fp_disp);
    const i64 lo = lower12(fp_disp);
    push(make_inst(MOp::Lui, 8, Operand{T0}, Operand{Imm{hi << 12}}));
    push(make_inst(MOp::Addi, 8, Operand{T0}, Operand{T0}, Operand{Imm{lo}}));
    push(make_inst(MOp::Add, 8, Operand{S1}, Operand{Reg::X2}, Operand{T0}));
  }

  for (MInst inst : body.insts) {
    pending_label = inst.label;
    inst.label = MInst::NO_LABEL;
    const bool floats = is_float_op(inst.op);
    const Reg* pool = floats ? FLOAT_SCRATCH : INTEGER_SCRATCH;
    const u32 pool_size = floats ? 8 : 7;
    const u8 load = 8;
    u32 next = 0;
    const bool write = writes_result(inst.op);
    std::optional<VReg> dest_vreg;

    // The address of a frame slot is materialized from sp, not loaded.
    if (inst.op == MOp::Addr && inst.count == 2) {
      const SlotRef* ref = std::get_if<SlotRef>(&inst.ops[1]);
      if (ref == nullptr) {
        continue;
      }
      const i64 disp = frame.vreg_disp[ref->vreg] + ref->offset;
      const Reg into = std::holds_alternative<VReg>(inst.ops[0])
                           ? pool[0]
                           : std::get<Reg>(inst.ops[0]);
      // The label names the start of this instruction. Keep it on the
      // first emitted word so a jump cannot skip the lui/addi prefix.
      const u32 label = pending_label;
      pending_label = MInst::NO_LABEL;
      if (fits_i12(disp)) {
        MInst addr = make_inst(MOp::Addi, 8, Operand{into}, Operand{Reg::X2},
                               Operand{Imm{disp}});
        addr.label = label;
        push(addr);
      } else {
        const i64 hi = upper20(disp);
        const i64 lo = lower12(disp);
        MInst hi_inst =
            make_inst(MOp::Lui, 8, Operand{into}, Operand{Imm{hi << 12}});
        hi_inst.label = label;
        push(hi_inst);
        push(make_inst(MOp::Addi, 8, Operand{into}, Operand{into},
                       Operand{Imm{lo}}));
        push(make_inst(MOp::Add, 8, Operand{into}, Operand{Reg::X2},
                       Operand{into}));
      }
      if (std::holds_alternative<VReg>(inst.ops[0])) {
        const VReg& dst = std::get<VReg>(inst.ops[0]);
        push(make_inst(MOp::Sd, 8, Operand{into}, Operand{slot(dst)}));
      }
      continue;
    }

    // An ALU form reads at most one immediate; materialize any later
    // one into a scratch register before the encoder sees it.
    u32 immediates_seen = 0;
    const bool all_register = needs_register_operands(inst.op);
    for (u32 at = 0; at < inst.count; ++at) {
      Operand& operand = inst.ops[at];
      if (const Imm* value = std::get_if<Imm>(&operand); value != nullptr) {
        // A store's value sits at operand 0 and must be a register.
        if (at == 0 || immediates_seen > 0 || all_register ||
            !fits_i12(value->value)) {
          const Reg into = pool[next++ % pool_size];
          const i64 v = value->value;
          const auto imm32 = [&](const Reg dst, const i32 word) {
            if (fits_i12(word)) {
              emit(make_inst(MOp::Addi, 8, Operand{dst}, Operand{ZERO},
                             Operand{Imm{word}}));
              return;
            }
            // Lui sign-extends bit 19 of its immediate. For a positive
            // 32-bit word truncate the high part; otherwise round up so
            // the sign-extended low part lands on the right bits.
            const i64 hi = word >= 0
                               ? word / 4096
                               : -((-static_cast<i64>(word) + 2047) / 4096);
            const i64 lo = static_cast<i64>(word) - hi * 4096;
            emit(make_inst(MOp::Lui, 8, Operand{dst}, Operand{Imm{hi << 12}}));
            if (lo > 2047) {
              // addi's immediate is signed 12-bit: split 0xfff into
              // 2047 + 2047 + 1 rather than letting 0x800 become -2048.
              emit(make_inst(MOp::Addi, 8, Operand{dst}, Operand{dst},
                             Operand{Imm{2047}}));
              emit(make_inst(MOp::Addi, 8, Operand{dst}, Operand{dst},
                             Operand{Imm{2047}}));
              emit(make_inst(MOp::Addi, 8, Operand{dst}, Operand{dst},
                             Operand{Imm{lo - 4094}}));
            } else if (lo != 0) {
              emit(make_inst(MOp::Addi, 8, Operand{dst}, Operand{dst},
                             Operand{Imm{lo}}));
            }
          };
          if (v >= INT32_MIN && v <= INT32_MAX) {
            imm32(into, static_cast<i32>(v));
          } else {
            // A 64-bit constant: build the low word, then the high word
            // in a scratch register and fold it in after a 32-bit shift.
            constexpr Reg high = Reg::X25;
            imm32(into, static_cast<i32>(static_cast<u64>(v)));
            // imm32 sign-extends; zero-extend the low word before adding.
            emit(make_inst(MOp::Slli, 8, Operand{into}, Operand{into},
                           Operand{Imm{32}}));
            emit(make_inst(MOp::Srli, 8, Operand{into}, Operand{into},
                           Operand{Imm{32}}));
            imm32(high, static_cast<i32>(static_cast<u64>(v) >> 32));
            emit(make_inst(MOp::Slli, 8, Operand{high}, Operand{high},
                           Operand{Imm{32}}));
            emit(make_inst(MOp::Add, 8, Operand{into}, Operand{into},
                           Operand{high}));
          }
          operand = Operand{into};
        } else {
          ++immediates_seen;
        }
        continue;
      }
      if (const SlotRef* slot_ref = std::get_if<SlotRef>(&operand);
          slot_ref != nullptr) {
        operand = address(frame.vreg_disp[slot_ref->vreg] + slot_ref->offset);
        continue;
      }
      if (const VReg* vreg = std::get_if<VReg>(&operand); vreg != nullptr) {
        if (at == 0 && write) {
          dest_vreg = *vreg;
        }
        const Reg reg = pool[next++ % pool_size];
        MOp load_mop = load_op(*vreg);
        if (floats) {
          load_mop = frame.vreg_size[vreg->index] == 4 ? MOp::Flw : MOp::Fld;
        }
        emit(make_inst(load_mop, load, Operand{reg}, Operand{slot(*vreg)}));
        operand = Operand{reg};
      } else if (Mem* mem = std::get_if<Mem>(&operand); mem != nullptr) {
        if (mem->base.has_value()) {
          const VReg* vreg = std::get_if<VReg>(&*mem->base);
          if (vreg == nullptr) {
            continue;
          }
          // A memory base is an integer address even in a float op; a
          // float scratch register cannot hold it.
          constexpr u32 integer_count = std::size(INTEGER_SCRATCH);
          const Reg reg = INTEGER_SCRATCH[next++ % integer_count];
          emit(make_inst(MOp::Ld, 8, Operand{reg}, Operand{slot(*vreg)}));
          mem->base = RegRef{reg};
        }
      }
    }

    emit(inst);

    if (write && dest_vreg.has_value()) {
      MOp store_mop = store_op(*dest_vreg);
      if (floats) {
        store_mop =
            frame.vreg_size[dest_vreg->index] == 4 ? MOp::Fsw : MOp::Fsd;
      }
      // Slots are at least a word wide. Clear the whole slot before a
      // narrow store so stale high bytes do not leak into a later wide
      // materialization (e.g. a byte-sized condition read by a branch).
      if (!floats && frame.vreg_size[dest_vreg->index] < 8) {
        emit(make_inst(MOp::Sd, 8, Operand{ZERO}, Operand{slot(*dest_vreg)}));
      }
      emit(make_inst(store_mop, 8, Operand{inst.ops[0]},
                     Operand{slot(*dest_vreg)}));
    }
  }

  if (body.uses_sret) {
    push(make_inst(MOp::Ld, 8, Operand{S0}, address(frame.size - 24)));
  }
  for (u32 i = 0; i < std::size(SAVED_REGS); ++i) {
    push(make_inst(MOp::Ld, 8, Operand{SAVED_REGS[i]},
                   address(save_disp(frame.size, i))));
  }
  if (big_frame) {
    const i64 hi = upper20(static_cast<i64>(frame.size));
    const i64 lo = lower12(static_cast<i64>(frame.size));
    push(make_inst(MOp::Lui, 8, Operand{T0}, Operand{Imm{hi << 12}}));
    push(make_inst(MOp::Addi, 8, Operand{T0}, Operand{T0}, Operand{Imm{lo}}));
    push(make_inst(MOp::Add, 8, Operand{Reg::X2}, Operand{Reg::X2},
                   Operand{T0}));
  } else {
    push(make_inst(MOp::Addi, 8, Operand{Reg::X2}, Operand{Reg::X2},
                   Operand{Imm{frame.size}}));
  }
  push(make_inst(MOp::Jalr, 8, Operand{ZERO}, Operand{RA}, Operand{Imm{0}}));
  body.insts = std::move(out);
}

}  // namespace codegen::riscv64
