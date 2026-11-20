// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/allocate.h"

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
    case MOp::RepMovsb:
    case MOp::RepStosb:
    case MOp::Ud2: return {};
  }
  return {};
}

// The registers allocation borrows, in the order reads take them. A
// divide reads its dividend and its sign out of `rax` and `rdx`, which
// the selector set up, so its one read takes `rcx` and only `rcx`.
constexpr Reg READ_SCRATCHES[] = {Reg::Rax, Reg::Rcx, Reg::Rdx};
constexpr Reg DIVISOR_SCRATCHES[] = {Reg::Rcx};

constexpr Mem slot(const i32 disp) {
  return Mem{.base = Reg::Rbp, .index = std::nullopt, .scale = 1, .disp = disp};
}

// Whether a value of this many bytes rides in one register, which is
// what decides a move's shape. Anything else crosses as bytes.
bool register_sized(const u32 size) {
  return size == 1 || size == 2 || size == 4 || size == 8;
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
    const u8 width = static_cast<u8>(frame.vreg_size[vreg.index]);
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
    if (inst.op == MOp::Ret) {
      // The exit, where the IR ended: the frame is given back and the
      // caller resumed.
      push_labeled(
          make_inst(MOp::Mov, 8, Operand{Reg::Rsp}, Operand{Reg::Rbp}));
      push_labeled(make_inst(MOp::Pop, 8, Operand{Reg::Rbp}));
      push_labeled(make_inst(MOp::Ret, 8));
      continue;
    }
    next_scratch = 0;
    pending_label = inst.label;
    inst.label = MInst::NO_LABEL;
    pool =
        inst.op == MOp::Idiv || inst.op == MOp::Udiv ? divisor_pool : read_pool;
    const Shape shape = shape_of(inst.op);

    // A move whose width no register holds runs as a byte copy between
    // the two addresses. Every other shape a slot wider than a register
    // could take is an aggregate read the selector spelled some other
    // way.
    const bool wide = shape.write0 &&
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
