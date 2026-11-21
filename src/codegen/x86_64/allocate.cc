// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/allocate.h"

#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "codegen/x86_64/frame.h"
#include "codegen/x86_64/isa.h"
#include "codegen/x86_64/minst.h"
#include "debug/dcheck.h"

namespace codegen::x86_64 {

namespace {
// What one opcode does with its operands. `read0` and `read1` say which
// operand positions are values the opcode reads, `write0` whether it
// leaves a result in the first; a read of the position it also writes is
// what makes an opcode two-operand.
struct Shape {
  bool read0 = false;
  bool read1 = false;
  bool write0 = false;
};

bool is_float_op(const MOp op) {
  switch (op) {
    case MOp::Movss:
    case MOp::Movsd:
    case MOp::Fadd:
    case MOp::Fsub:
    case MOp::Fmul:
    case MOp::Fdiv:
    case MOp::Fneg:
    case MOp::Fcom:
    case MOp::CvtI2F:
    case MOp::CvtF2I:
    case MOp::CvtF2F: return true;
    default: return false;
  }
}

Shape shape_of(const MOp op) {
  switch (op) {
    case MOp::Mov:
    case MOp::Movzx:
    case MOp::Movsx:
    case MOp::Lea: return {.read0 = false, .read1 = true, .write0 = true};
    case MOp::Add:
    case MOp::Sub:
    case MOp::And:
    case MOp::Or:
    case MOp::Xor:
    case MOp::Imul:
    case MOp::Shl:
    case MOp::Sar:
    case MOp::Shr: return {.read0 = true, .read1 = true, .write0 = true};
    case MOp::Not:
    case MOp::Neg: return {.read0 = true, .read1 = false, .write0 = true};
    case MOp::Cmp:
    case MOp::Test: return {.read0 = true, .read1 = true, .write0 = false};
    case MOp::Idiv:
    case MOp::Udiv:
    case MOp::Umul:
    case MOp::Push: return {.read0 = true, .read1 = false, .write0 = false};
    case MOp::Setcc:
    case MOp::Pop: return {.read0 = false, .read1 = false, .write0 = true};
    case MOp::Jmp:
    case MOp::Jcc:
    case MOp::Call:
    case MOp::CallReg:
    case MOp::Ret:
    case MOp::Syscall:
    case MOp::Nop:
    case MOp::Cqo:
    case MOp::Movss:
    case MOp::Movsd:
    case MOp::Fadd:
    case MOp::Fsub:
    case MOp::Fmul:
    case MOp::Fdiv:
    case MOp::Fneg:
    case MOp::Fcom:
    case MOp::CvtI2F:
    case MOp::CvtF2I:
    case MOp::CvtF2F:
    case MOp::RepMovsb:
    case MOp::RepStosb:
    case MOp::Ud2: return {};
  }
  return {};
}

// The registers allocation borrows, in the order reads take them. A
// divide reads its dividend and its sign out of `rax` and `rdx`, which
// the selector set up, so its one read takes `rcx` and only `rcx`.
constexpr Reg READ_SCRATCHES[] = {Reg::Rax, Reg::Rcx, Reg::Rdx, Reg::R10,
                                  Reg::R11};
constexpr Reg DIVISOR_SCRATCHES[] = {Reg::Rcx};

constexpr Mem slot(const i32 disp) {
  return Mem{.base = Reg::Rbp, .index = std::nullopt, .scale = 1, .disp = disp};
}

// Whether a value of this many bytes rides in one register, which is
// what decides a move's shape. Anything else crosses as bytes.
bool register_sized(const u32 size) {
  return size == 1 || size == 2 || size == 4 || size == 8;
}

// The register a memory operand's base names, which a copy needs before
// it runs.
Reg reg_of(const RegRef& ref) {
  return std::get<Reg>(ref);
}

}  // namespace

void allocate(Body& body, const Frame& frame) {
  std::vector<MInst> out;
  out.reserve(body.insts.size() + 8);

  // The entry sequence. A frame pointer stands on every function so a
  // panic walk reads the chain, and `rsp` lands 16-aligned: the call
  // pushed a return address, the push undid it.
  out.push_back(make_inst(MOp::Push, 8, Operand{Reg::Rbp}));
  out.push_back(make_inst(MOp::Mov, 8, Operand{Reg::Rbp}, Operand{Reg::Rsp}));
  if (frame.size != 0) {
    out.push_back(
        make_inst(MOp::Sub, 8, Operand{Reg::Rsp}, Operand{Imm{frame.size}}));
  }

  u32 next_scratch = 0;
  const std::span<const Reg> read_pool = std::span<const Reg>(READ_SCRATCHES);
  // A divide's one read takes `rcx`, the register neither of the two the
  // opcode reads its dividend and its sign out of.
  const std::span<const Reg> divisor_pool =
      std::span<const Reg>(DIVISOR_SCRATCHES);
  // The pool this instruction's reads borrow from, rebound per
  // instruction.
  std::span<const Reg> pool = read_pool;

  // The label the instruction being lowered opens, held until the first
  // byte of its lowering is written. Materialization emits before the
  // opcode it serves, so a block's label belongs on that first byte, not
  // on the opcode that follows it.
  u32 pending_label = MInst::NO_LABEL;
  const auto push_labeled = [&](MInst inst) {
    if (pending_label != MInst::NO_LABEL) {
      inst.label = pending_label;
      pending_label = MInst::NO_LABEL;
    }
    out.push_back(inst);
  };

  const auto pointee_of = [&](const VReg vreg) -> std::optional<i32> {
    if (vreg.index < frame.vreg_pointee.size() &&
        frame.vreg_pointee[vreg.index] != Frame::NO_POINTEE) {
      return frame.vreg_pointee[vreg.index];
    }
    return std::nullopt;
  };

  // Brings one vreg's value into a scratch, reading the slot at the
  // width the value holds: a narrower read zero-extends, so the high
  // bytes a wider one would carry are never there to mistake for data.
  const auto load_value = [&](const VReg vreg) -> Reg {
    const Reg into = pool[next_scratch++];
    DCHECK(next_scratch <= pool.size());
    if (const std::optional<i32> pointee = pointee_of(vreg);
        pointee.has_value()) {
      push_labeled(
          make_inst(MOp::Lea, 8, Operand{into}, Operand{slot(*pointee)}));
      return into;
    }
    DCHECK(vreg.index < frame.vreg_disp.size());
    DCHECK(vreg.index < frame.vreg_size.size());
    // A register holds one word; a wider slot's later words are named
    // by `SlotRef` with an offset, so the first word is all a plain read
    // of the vreg can ask for.
    const u8 width = static_cast<u8>(
        frame.vreg_size[vreg.index] > 8 ? 8 : frame.vreg_size[vreg.index]);
    const Mem from = slot(frame.vreg_disp[vreg.index]);
    if (width == 1 || width == 2) {
      // A narrow value extends itself the way its type reads: signed to
      // its sign, unsigned to zero, so the 32-bit opcodes that follow
      // see the value the source wrote.
      const MOp widen = frame.vreg_signed[vreg.index] ? MOp::Movsx : MOp::Movzx;
      push_labeled(make_inst(widen, width, Operand{into}, Operand{from}));
    } else {
      push_labeled(make_inst(MOp::Mov, width == 3 ? 4 : width, Operand{into},
                             Operand{from}));
    }
    return into;
  };

  // Brings one vreg's *address* into a scratch, which is the only thing
  // a slot wider than a register offers.
  const auto load_address = [&](const VReg vreg) -> Reg {
    const Reg into = pool[next_scratch++];
    DCHECK(next_scratch <= pool.size());
    if (const std::optional<i32> pointee = pointee_of(vreg);
        pointee.has_value()) {
      push_labeled(
          make_inst(MOp::Lea, 8, Operand{into}, Operand{slot(*pointee)}));
    } else {
      DCHECK(vreg.index < frame.vreg_disp.size());
      push_labeled(make_inst(MOp::Lea, 8, Operand{into},
                             Operand{slot(frame.vreg_disp[vreg.index])}));
    }
    return into;
  };

  // The frame displacement one vreg's storage starts at, which a slot
  // reference is relative to. A vreg an `alloca` defines names its own
  // storage, so that is the base its fields read from.
  const auto disp_of = [&](const u32 vreg) -> i32 {
    if (vreg < frame.vreg_pointee.size() &&
        frame.vreg_pointee[vreg] != Frame::NO_POINTEE) {
      return frame.vreg_pointee[vreg];
    }
    return frame.vreg_disp[vreg];
  };

  // Rewrites one read operand: a vreg becomes the register it was
  // brought into, a memory operand's vreg base and index theirs, and a
  // slot reference the frame reference it names.
  const auto resolve = [&](const Operand& operand) -> Operand {
    if (const VReg* vreg = std::get_if<VReg>(&operand); vreg != nullptr) {
      return Operand{load_value(*vreg)};
    }
    if (const Mem* mem = std::get_if<Mem>(&operand); mem != nullptr) {
      Mem resolved = *mem;
      if (resolved.base.has_value()) {
        if (const VReg* vreg = std::get_if<VReg>(&*resolved.base);
            vreg != nullptr) {
          resolved.base = load_value(*vreg);
        }
      }
      if (resolved.index.has_value()) {
        if (const VReg* vreg = std::get_if<VReg>(&*resolved.index);
            vreg != nullptr) {
          resolved.index = load_value(*vreg);
        }
      }
      return Operand{resolved};
    }
    if (const SlotRef* ref = std::get_if<SlotRef>(&operand); ref != nullptr) {
      return Operand{slot(static_cast<i32>(disp_of(ref->vreg) + ref->offset))};
    }
    return operand;
  };

  // The bytes one vreg's slot holds, which is what decides a move's
  // shape.
  const auto size_of = [&](const VReg vreg) -> u32 {
    DCHECK(vreg.index < frame.vreg_size.size());
    return frame.vreg_size[vreg.index];
  };

  for (MInst& inst : body.insts) {
    next_scratch = 0;
    pending_label = inst.label;
    inst.label = MInst::NO_LABEL;
    if (inst.op == MOp::Ret) {
      // The exit, where the IR ended: the frame is given back and the
      // caller resumed.
      push_labeled(
          make_inst(MOp::Mov, 8, Operand{Reg::Rsp}, Operand{Reg::Rbp}));
      push_labeled(make_inst(MOp::Pop, 8, Operand{Reg::Rbp}));
      push_labeled(make_inst(MOp::Ret, 8));
      continue;
    }
    pool =
        inst.op == MOp::Idiv || inst.op == MOp::Udiv ? divisor_pool : read_pool;
    const Shape shape = shape_of(inst.op);

    // Floating-point operations read their operands from the frame and
    // write back to it, so the XMM file needs no allocation: two scratch
    // registers carry every binary form. A float constant is named by a
    // symbol whose bytes the read-only section holds; a GPR borrows its
    // address.
    if (is_float_op(inst.op)) {
      const u8 w = inst.width;
      const MOp load_op = w == 4 ? MOp::Movss : MOp::Movsd;
      constexpr Reg X = Reg::Xmm0;
      constexpr Reg Y = Reg::Xmm1;
      const auto load_float = [&](const Operand& src, Reg into) {
        if (const VReg* vreg = std::get_if<VReg>(&src); vreg != nullptr) {
          push_labeled(make_inst(load_op, w, Operand{into},
                                 Operand{slot(disp_of(vreg->index))}));
        } else if (const SymbolRef* sym = std::get_if<SymbolRef>(&src);
                   sym != nullptr) {
          const Reg addr = pool[next_scratch++];
          DCHECK(next_scratch <= pool.size());
          push_labeled(make_inst(MOp::Mov, 8, Operand{addr}, Operand{*sym}));
          push_labeled(make_inst(load_op, w, Operand{into},
                                 Operand{Mem{.base = addr,
                                             .index = std::nullopt,
                                             .scale = 1,
                                             .disp = 0}}));
        } else if (const Mem* mem = std::get_if<Mem>(&src); mem != nullptr) {
          push_labeled(make_inst(load_op, w, Operand{into}, resolve(*mem)));
        } else if (const Reg* from = std::get_if<Reg>(&src); from != nullptr) {
          if (*from != into) {
            push_labeled(make_inst(load_op, w, Operand{into}, Operand{*from}));
          }
        }
      };
      const auto store_float = [&](const Operand& dst, Reg from) {
        if (const VReg* vreg = std::get_if<VReg>(&dst); vreg != nullptr) {
          const u8 dst_w = frame.vreg_size[vreg->index] == 4 ? 4 : 8;
          const MOp store_op = dst_w == 4 ? MOp::Movss : MOp::Movsd;
          push_labeled(make_inst(store_op, dst_w,
                                 Operand{slot(disp_of(vreg->index))},
                                 Operand{from}));
        } else if (const Mem* mem = std::get_if<Mem>(&dst); mem != nullptr) {
          push_labeled(make_inst(load_op, w, resolve(*mem), Operand{from}));
        } else if (const Reg* to = std::get_if<Reg>(&dst); to != nullptr) {
          if (*to != from) {
            push_labeled(make_inst(load_op, w, Operand{*to}, Operand{from}));
          }
        }
      };
      if (inst.op == MOp::Fcom) {
        load_float(inst.ops[0], X);
        load_float(inst.ops[1], Y);
        push_labeled(make_inst(MOp::Fcom, w, Operand{X}, Operand{Y}));
        continue;
      }
      if (inst.op == MOp::CvtI2F) {
        const Reg from = load_value(std::get<VReg>(inst.ops[1]));
        push_labeled(make_inst(MOp::CvtI2F, w, Operand{X}, Operand{from}));
        store_float(inst.ops[0], X);
        continue;
      }
      if (inst.op == MOp::CvtF2I) {
        load_float(inst.ops[1], X);
        const VReg dst = std::get<VReg>(inst.ops[0]);
        const u8 out_width = static_cast<u8>(
            frame.vreg_size[dst.index] > 8 ? 8 : frame.vreg_size[dst.index]);
        // `w` names the source float width (ss/sd opcode); the third
        // operand names the destination type as a signed byte count,
        // negative when the type is unsigned.
        const i64 tagged = std::get<Imm>(inst.ops[2]).value;
        const bool uns = tagged < 0;
        const i64 type_bytes = uns ? -tagged : tagged;
        const u8 cvt_width = out_width < 4 ? 4 : out_width;
        const i64 conv_min = cvt_width == 4 ? INT32_MIN : INT64_MIN;
        const i64 type_min = !uns ? (type_bytes == 1   ? -128
                                     : type_bytes == 2 ? -32768
                                                       : conv_min)
                                  : 0;
        const i64 type_max =
            uns ? (type_bytes == 8   ? -1
                   : type_bytes == 4 ? 4294967295LL
                   : type_bytes == 2 ? 65535
                                     : 255)
                : (type_bytes == 1 ? 127
                   : type_bytes == 2
                       ? 32767
                       : (cvt_width == 4 ? INT32_MAX : INT64_MAX));

        push_labeled(make_inst(MOp::CvtF2I, w, Operand{Reg::Rax}, Operand{X},
                               Operand{Imm{static_cast<i64>(out_width)}}));
        const u32 done = body.label_count++;

        // NaN: cvtt returns conv_min, but comisd x,x sets PF for a NaN.
        push_labeled(make_inst(MOp::Fcom, w, Operand{X}, Operand{X}));
        const u32 not_nan = body.label_count++;
        push_labeled(make_jcc(Cond::Np, Operand{Label{not_nan}}));
        push_labeled(make_inst(MOp::Xor, out_width, Operand{Reg::Rax},
                               Operand{Reg::Rax}));
        push_labeled(make_inst(MOp::Jmp, 8, Operand{Label{done}}));
        MInst at_not_nan = make_inst(MOp::Nop, 8);
        at_not_nan.label = not_nan;
        push_labeled(at_not_nan);

        // conv_min is the sentinel for a positive overflow (source >= 0)
        // or, for signed types, a negative overflow (source < 0).
        const u32 not_sentinel = body.label_count++;
        push_labeled(make_inst(MOp::Mov, cvt_width, Operand{Reg::R9},
                               Operand{Imm{conv_min}}));
        push_labeled(make_inst(MOp::Cmp, cvt_width, Operand{Reg::Rax},
                               Operand{Reg::R9}));
        push_labeled(make_jcc(Cond::Ne, Operand{Label{not_sentinel}}));
        // Compare the source with +0.0 to tell the two apart.
        push_labeled(
            make_inst(MOp::Xor, 4, Operand{Reg::R8}, Operand{Reg::R8}));
        push_labeled(make_inst(MOp::CvtI2F, w == 4 ? 4 : 8, Operand{Reg::Xmm2},
                               Operand{Reg::R8}));
        push_labeled(make_inst(MOp::Fcom, w, Operand{X}, Operand{Reg::Xmm2}));
        const u32 positive = body.label_count++;
        push_labeled(make_jcc(Cond::Ae, Operand{Label{positive}}));
        // negative overflow: answer the type minimum.
        push_labeled(make_inst(MOp::Mov, out_width, Operand{Reg::Rax},
                               Operand{Imm{type_min}}));
        push_labeled(make_inst(MOp::Jmp, 8, Operand{Label{done}}));
        MInst at_positive = make_inst(MOp::Nop, 8);
        at_positive.label = positive;
        push_labeled(at_positive);
        // positive overflow: answer the type maximum.
        push_labeled(make_inst(MOp::Mov, cvt_width, Operand{Reg::R11},
                               Operand{Imm{type_max}}));
        push_labeled(make_inst(MOp::Mov, out_width, Operand{Reg::Rax},
                               Operand{Reg::R11}));
        push_labeled(make_inst(MOp::Jmp, 8, Operand{Label{done}}));
        MInst at_not_sentinel = make_inst(MOp::Nop, 8);
        at_not_sentinel.label = not_sentinel;
        push_labeled(at_not_sentinel);

        // Unsigned narrow types: a negative result or one past the type
        // maximum saturates to 0 or the maximum respectively.
        if (uns && type_bytes < 8) {
          const u32 nonnegative = body.label_count++;
          push_labeled(make_inst(MOp::Test, cvt_width, Operand{Reg::Rax},
                                 Operand{Reg::Rax}));
          push_labeled(make_jcc(Cond::Ns, Operand{Label{nonnegative}}));
          push_labeled(make_inst(MOp::Xor, out_width, Operand{Reg::Rax},
                                 Operand{Reg::Rax}));
          push_labeled(make_inst(MOp::Jmp, 8, Operand{Label{done}}));
          MInst at_nonnegative = make_inst(MOp::Nop, 8);
          at_nonnegative.label = nonnegative;
          push_labeled(at_nonnegative);
          push_labeled(make_inst(MOp::Mov, cvt_width, Operand{Reg::R11},
                                 Operand{Imm{type_max}}));
          push_labeled(make_inst(MOp::Cmp, cvt_width, Operand{Reg::Rax},
                                 Operand{Reg::R11}));
          const u32 within = body.label_count++;
          push_labeled(make_jcc(Cond::Be, Operand{Label{within}}));
          push_labeled(make_inst(MOp::Mov, out_width, Operand{Reg::Rax},
                                 Operand{Reg::R11}));
          MInst at_within = make_inst(MOp::Nop, 8);
          at_within.label = within;
          push_labeled(at_within);
        }

        MInst at_done = make_inst(MOp::Nop, 8);
        at_done.label = done;
        push_labeled(at_done);
        push_labeled(make_inst(MOp::Mov, out_width,
                               Operand{slot(disp_of(dst.index))},
                               Operand{Reg::Rax}));
        continue;
      }
      if (inst.op == MOp::Fneg) {
        load_float(inst.ops[1], X);
        load_float(inst.ops[2], Y);
        push_labeled(make_inst(MOp::Fneg, w, Operand{X}, Operand{Y}));
        store_float(inst.ops[0], X);
        continue;
      }
      if (inst.op == MOp::CvtF2F) {
        load_float(inst.ops[1], X);
        push_labeled(make_inst(MOp::CvtF2F, w, Operand{Y}, Operand{X}));
        store_float(inst.ops[0], Y);
        continue;
      }
      // Operand order from the selector is dst, lhs, rhs.
      if (inst.op == MOp::Movss || inst.op == MOp::Movsd) {
        load_float(inst.ops[1], Y);
        store_float(inst.ops[0], Y);
      } else {
        load_float(inst.ops[1], X);
        load_float(inst.ops[2], Y);
        push_labeled(make_inst(inst.op, w, Operand{X}, Operand{Y}));
        store_float(inst.ops[0], X);
      }
      continue;
    }

    // A move whose width no register holds runs as a byte copy between
    // the two addresses. Every other shape a slot wider than a register
    // could take is an aggregate read the selector spelled some other
    // way.
    const bool wide = inst.op == MOp::Mov && shape.write0 &&
                      std::holds_alternative<VReg>(inst.ops[0]) &&
                      !register_sized(size_of(std::get<VReg>(inst.ops[0])));
    if (wide && size_of(std::get<VReg>(inst.ops[0])) == 0) {
      // A value with no bytes has nothing to move.
      continue;
    }
    if (wide) {
      DCHECK_MSG(inst.op == MOp::Mov,
                 "only a move crosses a slot wider than a register");
      const VReg dst = std::get<VReg>(inst.ops[0]);
      push_labeled(make_inst(MOp::Lea, 8, Operand{Reg::Rdi},
                             Operand{slot(frame.vreg_disp[dst.index])}));
      if (const VReg* src = std::get_if<VReg>(&inst.ops[1]); src != nullptr) {
        // Slot to slot: both sides are addresses.
        const Reg from = load_address(*src);
        if (from != Reg::Rsi) {
          push_labeled(
              make_inst(MOp::Mov, 8, Operand{Reg::Rsi}, Operand{from}));
        }
      } else if (const Mem* src = std::get_if<Mem>(&inst.ops[1]);
                 src != nullptr && src->base.has_value()) {
        // Through a pointer the selector held: its value is the source.
        const Reg from = load_value(std::get<VReg>(*src->base));
        if (from != Reg::Rsi) {
          push_labeled(
              make_inst(MOp::Mov, 8, Operand{Reg::Rsi}, Operand{from}));
        }
      } else if (std::holds_alternative<SlotRef>(inst.ops[1])) {
        // The slot the selector named: its frame reference is the
        // source the copy reads.
        const Operand resolved = resolve(inst.ops[1]);
        const auto& base = std::get<Mem>(resolved).base;
        DCHECK(base.has_value());
        (void)reg_of(*base);
        push_labeled(make_inst(MOp::Mov, 8, Operand{Reg::Rsi}, resolved));
      } else {
        DCHECK_MSG(false, "a wide move reads an address");
      }
      push_labeled(make_inst(MOp::Mov, 8, Operand{Reg::Rcx},
                             Operand{Imm{size_of(dst)}}));
      push_labeled(make_inst(MOp::RepMovsb, 8));
      continue;
    }

    // A move of a value no register holds, into the memory a pointer
    // names: the pointer is the destination, the slot the source, and
    // the bytes cross whole.
    if (inst.op == MOp::Mov && std::holds_alternative<Mem>(inst.ops[0]) &&
        std::holds_alternative<VReg>(inst.ops[1]) &&
        !register_sized(size_of(std::get<VReg>(inst.ops[1])))) {
      const Mem& destination = std::get<Mem>(inst.ops[0]);
      const Reg to = load_value(
          std::get<VReg>(destination.base.value_or(RegRef{Reg::Rdi})));
      push_labeled(make_inst(MOp::Mov, 8, Operand{Reg::Rdi}, Operand{to}));
      const Reg from = load_address(std::get<VReg>(inst.ops[1]));
      if (from != Reg::Rsi) {
        push_labeled(make_inst(MOp::Mov, 8, Operand{Reg::Rsi}, Operand{from}));
      }
      push_labeled(
          make_inst(MOp::Mov, 8, Operand{Reg::Rcx},
                    Operand{Imm{size_of(std::get<VReg>(inst.ops[1]))}}));
      push_labeled(make_inst(MOp::RepMovsb, 8));
      continue;
    }

    // A move into a register the selector named takes its source there
    // directly: the argument registers a call reads are filled this way.
    if (inst.op == MOp::Mov && std::holds_alternative<Reg>(inst.ops[0])) {
      const Reg dst = std::get<Reg>(inst.ops[0]);
      push_labeled(
          make_inst(MOp::Mov, inst.width, Operand{dst}, resolve(inst.ops[1])));
      continue;
    }

    MInst encoded = inst;
    // Reads resolve first, destination first, so a two-operand opcode
    // finds its own value in the register it will write. A write into a
    // memory operand resolves its base the same way.
    // A write into memory resolves the reference it writes through,
    // which a slot reference spells as plainly as a pointer does.
    if (shape.read0 ||
        (shape.write0 && (std::holds_alternative<Mem>(inst.ops[0]) ||
                          std::holds_alternative<SlotRef>(inst.ops[0])))) {
      encoded.ops[0] = resolve(inst.ops[0]);
    }
    if (shape.read1) {
      encoded.ops[1] = resolve(inst.ops[1]);
    }

    // A divide reads its divisor from a register, so a constant takes
    // the one the dividend and its sign are not in.
    if ((encoded.op == MOp::Idiv || encoded.op == MOp::Udiv) &&
        std::holds_alternative<Imm>(encoded.ops[0])) {
      const Reg into = pool[next_scratch++];
      DCHECK(next_scratch <= pool.size());
      push_labeled(make_inst(MOp::Mov, 8, Operand{into}, encoded.ops[0]));
      encoded.ops[0] = Operand{into};
    }
    // A store of a symbol address needs the address in a register first:
    // x86 has no memory-to-immediate form that carries a relocation.
    if (encoded.op == MOp::Mov && std::holds_alternative<Mem>(encoded.ops[0]) &&
        std::holds_alternative<SymbolRef>(encoded.ops[1])) {
      const Reg into = pool[next_scratch++];
      DCHECK(next_scratch <= pool.size());
      push_labeled(make_inst(MOp::Mov, 8, Operand{into}, encoded.ops[1]));
      encoded.ops[1] = Operand{into};
    }
    // A widening move reads its source from a register or memory; a
    // constant needs a scratch the instruction may clobber.
    if ((encoded.op == MOp::Movsx || encoded.op == MOp::Movzx) &&
        std::holds_alternative<Imm>(encoded.ops[1])) {
      const Reg into = pool[next_scratch++];
      DCHECK(next_scratch <= pool.size());
      push_labeled(
          make_inst(MOp::Mov, inst.width, Operand{into}, encoded.ops[1]));
      encoded.ops[1] = Operand{into};
    }
    // An x86 scale field holds only 1, 2, 4 or 8. A wider element needs
    // its index shifted first, then the scaled address uses one byte per
    // step.
    if (encoded.op == MOp::Lea) {
      if (Mem* mem = std::get_if<Mem>(&encoded.ops[1]);
          mem != nullptr && mem->index.has_value() && mem->scale > 8) {
        const Reg into = pool[next_scratch++];
        DCHECK(next_scratch <= pool.size());
        const Reg idx = std::get<Reg>(*mem->index);
        push_labeled(make_inst(MOp::Mov, 8, Operand{into}, Operand{idx}));
        if (std::has_single_bit(mem->scale)) {
          const i64 shift = static_cast<i64>(std::countr_zero(mem->scale));
          push_labeled(
              make_inst(MOp::Shl, 8, Operand{into}, Operand{Imm{shift}}));
        } else {
          const Reg factor = pool[next_scratch++];
          DCHECK(next_scratch <= pool.size());
          push_labeled(make_inst(MOp::Mov, 8, Operand{factor},
                                 Operand{Imm{static_cast<i64>(mem->scale)}}));
          push_labeled(make_inst(MOp::Imul, 8, Operand{into}, Operand{factor}));
        }
        mem->index = into;
        mem->scale = 1;
      }
    }
    // The compare and ALU forms read their first operand from a register;
    // a constant there needs a scratch the instruction may clobber.
    if ((encoded.op == MOp::Cmp || encoded.op == MOp::Add ||
         encoded.op == MOp::Sub || encoded.op == MOp::And ||
         encoded.op == MOp::Or || encoded.op == MOp::Xor ||
         encoded.op == MOp::Imul || encoded.op == MOp::Shl ||
         encoded.op == MOp::Shr || encoded.op == MOp::Sar ||
         encoded.op == MOp::Not || encoded.op == MOp::Neg) &&
        std::holds_alternative<Imm>(encoded.ops[0])) {
      const Reg into = pool[next_scratch++];
      DCHECK(next_scratch <= pool.size());
      push_labeled(
          make_inst(MOp::Mov, inst.width, Operand{into}, encoded.ops[0]));
      encoded.ops[0] = Operand{into};
    }
    std::optional<Reg> result;
    if (shape.write0 && std::holds_alternative<VReg>(inst.ops[0])) {
      if (const Reg* named = std::get_if<Reg>(&encoded.ops[0]);
          named != nullptr) {
        // A register the selector chose, or the scratch a read brought
        // the destination into: the opcode writes it as it stands.
        result = *named;
      } else {
        result = pool[next_scratch++];
        DCHECK(next_scratch <= pool.size());
        encoded.ops[0] = Operand{*result};
      }
    }
    // A store's immediate carries 32 bits, so a wider constant reaches
    // the slot through a register this instruction is free to spend.
    if (encoded.op == MOp::Mov && std::holds_alternative<Mem>(encoded.ops[0]) &&
        std::holds_alternative<Imm>(encoded.ops[1])) {
      const Imm& value = std::get<Imm>(encoded.ops[1]);
      if (value.value > 0x7fffffffLL || value.value < -0x80000000LL) {
        const Reg into = pool[next_scratch++];
        DCHECK(next_scratch <= pool.size());
        push_labeled(make_inst(MOp::Mov, 8, Operand{into}, encoded.ops[1]));
        encoded.ops[1] = Operand{into};
      }
    }
    push_labeled(encoded);

    // The result reaches the slot it came from, in the width the slot
    // holds.
    if (result.has_value() && std::holds_alternative<VReg>(inst.ops[0])) {
      const VReg dst = std::get<VReg>(inst.ops[0]);
      DCHECK(dst.index < frame.vreg_disp.size());
      push_labeled(make_inst(MOp::Mov, static_cast<u8>(size_of(dst)),
                             Operand{slot(frame.vreg_disp[dst.index])},
                             Operand{*result}));
    }
  }

  body.insts = std::move(out);
}

}  // namespace codegen::x86_64
