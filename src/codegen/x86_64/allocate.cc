// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/allocate.h"

#include <optional>
#include <utility>
#include <variant>
#include <vector>

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
    case MOp::Push: return {.read0 = false, .read1 = true, .write0 = false};
    case MOp::Setcc:
    case MOp::Pop: return {.read0 = false, .read1 = false, .write0 = true};
    case MOp::Jmp:
    case MOp::Jcc:
    case MOp::Call:
    case MOp::Ret:
    case MOp::Cqo:
    case MOp::RepMovsb:
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

  const auto pointee_of = [&](const VReg vreg) -> std::optional<i32> {
    if (vreg.index < frame.vreg_pointee.size() &&
        frame.vreg_pointee[vreg.index] >= 0) {
      return frame.vreg_pointee[vreg.index];
    }
    return std::nullopt;
  };

  // Brings one vreg's value into a scratch. The load reads a whole
  // register's worth of the slot: the store back truncates, and the
  // narrow opcodes re-derive what the high bytes carry.
  const auto load_value = [&](const VReg vreg) -> Reg {
    const Reg into = pool[next_scratch++];
    DCHECK(next_scratch <= pool.size());
    if (const std::optional<i32> pointee = pointee_of(vreg);
        pointee.has_value()) {
      out.push_back(
          make_inst(MOp::Lea, 8, Operand{into}, Operand{slot(*pointee)}));
    } else {
      DCHECK(vreg.index < frame.vreg_disp.size());
      out.push_back(make_inst(MOp::Mov, 8, Operand{into},
                              Operand{slot(frame.vreg_disp[vreg.index])}));
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
      out.push_back(
          make_inst(MOp::Lea, 8, Operand{into}, Operand{slot(*pointee)}));
    } else {
      DCHECK(vreg.index < frame.vreg_disp.size());
      out.push_back(make_inst(MOp::Lea, 8, Operand{into},
                              Operand{slot(frame.vreg_disp[vreg.index])}));
    }
    return into;
  };

  // Rewrites one read operand: a vreg becomes the register it was
  // brought into, and a memory operand's vreg base and index theirs.
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
      out.push_back(
          make_inst(MOp::Mov, 8, Operand{Reg::Rsp}, Operand{Reg::Rbp}));
      out.push_back(make_inst(MOp::Pop, 8, Operand{Reg::Rbp}));
      out.push_back(make_inst(MOp::Ret, 8));
      continue;
    }
    next_scratch = 0;
    pool =
        inst.op == MOp::Idiv || inst.op == MOp::Udiv ? divisor_pool : read_pool;
    const Shape shape = shape_of(inst.op);

    // A move whose width no register holds runs as a byte copy between
    // the two addresses. Every other shape a slot wider than a register
    // could take is an aggregate read the selector spelled some other
    // way.
    const bool wide = shape.write0 &&
                      std::holds_alternative<VReg>(inst.ops[0]) &&
                      size_of(std::get<VReg>(inst.ops[0])) > 8;
    if (wide) {
      DCHECK_MSG(inst.op == MOp::Mov,
                 "only a move crosses a slot wider than a register");
      const VReg dst = std::get<VReg>(inst.ops[0]);
      out.push_back(make_inst(MOp::Lea, 8, Operand{Reg::Rdi},
                              Operand{slot(frame.vreg_disp[dst.index])}));
      if (const VReg* src = std::get_if<VReg>(&inst.ops[1]); src != nullptr) {
        // Slot to slot: both sides are addresses.
        const Reg from = load_address(*src);
        if (from != Reg::Rsi) {
          out.push_back(
              make_inst(MOp::Mov, 8, Operand{Reg::Rsi}, Operand{from}));
        }
      } else if (const Mem* src = std::get_if<Mem>(&inst.ops[1]);
                 src != nullptr && src->base.has_value()) {
        // Through a pointer the selector held: its value is the source.
        const Reg from = load_value(std::get<VReg>(*src->base));
        if (from != Reg::Rsi) {
          out.push_back(
              make_inst(MOp::Mov, 8, Operand{Reg::Rsi}, Operand{from}));
        }
      } else {
        DCHECK_MSG(false, "a wide move reads an address");
      }
      out.push_back(make_inst(MOp::Mov, 8, Operand{Reg::Rcx},
                              Operand{Imm{size_of(dst)}}));
      out.push_back(make_inst(MOp::RepMovsb, 8));
      continue;
    }

    // A move into a register the selector named takes its source there
    // directly: the argument registers a call reads are filled this way.
    if (inst.op == MOp::Mov && std::holds_alternative<Reg>(inst.ops[0])) {
      const Reg dst = std::get<Reg>(inst.ops[0]);
      out.push_back(
          make_inst(MOp::Mov, inst.width, Operand{dst}, resolve(inst.ops[1])));
      continue;
    }

    MInst encoded = inst;
    // Reads resolve first, destination first, so a two-operand opcode
    // finds its own value in the register it will write. A write into a
    // memory operand resolves its base the same way.
    if (shape.read0 ||
        (shape.write0 && std::holds_alternative<Mem>(inst.ops[0]))) {
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
    out.push_back(std::move(encoded));

    // The result reaches the slot it came from, in the width the slot
    // holds.
    if (result.has_value() && std::holds_alternative<VReg>(inst.ops[0])) {
      const VReg dst = std::get<VReg>(inst.ops[0]);
      DCHECK(dst.index < frame.vreg_disp.size());
      out.push_back(make_inst(MOp::Mov, static_cast<u8>(size_of(dst)),
                              Operand{slot(frame.vreg_disp[dst.index])},
                              Operand{*result}));
    }
  }

  body.insts = std::move(out);
}

}  // namespace codegen::x86_64
