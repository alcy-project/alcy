// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/select.h"

#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "codegen/backend.h"
#include "codegen/diag_code.h"
#include "debug/dcheck.h"
#include "i18n/messages.h"
#include "ir/opcode.h"

namespace codegen::x86_64 {

namespace {

// The bytes one scalar value occupies, which is what the integer
// opcodes' width field asks for. Arithmetic runs at 32 or 64 bits and
// stores at the value's own width, so the narrow widths name moves and
// nothing else.
u8 width_of(const ir::TypeTag tag) {
  switch (tag) {
    case ir::TypeTag::I1:
    case ir::TypeTag::I8:
    case ir::TypeTag::U8: return 1;
    case ir::TypeTag::I16:
    case ir::TypeTag::U16: return 2;
    case ir::TypeTag::I32:
    case ir::TypeTag::U32:
    case ir::TypeTag::F32: return 4;
    default: return 8;
  }
}

class Selector {
 public:
  Selector(const ir::Storage& storage,
           std::span<const diag::Span> spans,
           const ir::Function& function,
           diag::DiagBag& bag,
           const ir::PointerWidth width)
      : storage_(storage),
        spans_(spans),
        function_(function),
        bag_(bag),
        width_(width) {}

  base::Result<Body, codegen::EmitError> run();

 private:
  // A construct with no lowering, at the place it was written.
  base::Result<Body, codegen::EmitError> unsupported(const diag::Span span,
                                                     std::string_view what);

  // The vreg one IR register selects to, making one on first use. The
  // register indexes are the module's, so the map is what keeps a body's
  // vregs dense.
  u32 vreg_of(ir::RegisterIdx reg, ir::TypeIdx type);

  diag::Span span_of(const ir::InstructionIdx index) const {
    return index.idx < spans_.size() ? spans_[index.idx] : diag::Span{};
  }

  const ir::Storage& storage_;
  std::span<const diag::Span> spans_;
  const ir::Function& function_;
  diag::DiagBag& bag_;
  ir::PointerWidth width_;

  Body body_;
  std::unordered_map<u32, u32> vreg_of_register_;
  bool failed_ = false;
};

u32 Selector::vreg_of(const ir::RegisterIdx reg, const ir::TypeIdx type) {
  const auto found = vreg_of_register_.find(reg.idx);
  if (found != vreg_of_register_.end()) {
    return found->second;
  }
  const u32 index = static_cast<u32>(body_.vreg_types.size());
  body_.vreg_types.push_back(type.idx);
  vreg_of_register_.emplace(reg.idx, index);
  return index;
}

base::Result<Body, codegen::EmitError> Selector::unsupported(
    const diag::Span span,
    const std::string_view what) {
  if (!failed_) {
    (void)bag_.emit<i18n::Key::CodegenX86Unsupported>(
        codegen::DiagCode::Unsupported, span, what);
    failed_ = true;
  }
  return base::make_err(codegen::EmitError::Unsupported);
}

base::Result<Body, codegen::EmitError> Selector::run() {
  // Every block the body carries gets a label, whether or not a branch
  // names it: a block's first instruction is where its label sits, and
  // the map from block to label is what the branches read.
  const u32 block_count = static_cast<u32>(function_.blocks.size());
  body_.block_labels.assign(block_count, 0);
  for (u32 at = 0; at < block_count; ++at) {
    body_.block_labels[at] = body_.label_count++;
  }

  for (const ir::BlockIdx block_idx : function_.blocks) {
    const ir::Block& block = storage_.blocks()[block_idx];
    const u32 label = body_.block_labels[block_idx.idx];
    bool placed = false;
    const auto push = [&](MInst inst) {
      if (!placed) {
        inst.label = label;
        placed = true;
      }
      body_.insts.push_back(std::move(inst));
    };

    for (const ir::InstructionIdx instr_idx : block.instrs) {
      const ir::Instruction& instr = storage_.instrs()[instr_idx];
      const diag::Span span = span_of(instr_idx);
      const ir::OperandIdxRange ops = instr.operands;

      switch (instr.op) {
        case ir::Opcode::Noop: {
          continue;
        }
        case ir::Opcode::Ret: {
          // An explicit `()` comes back as an operand of a type that
          // holds no bytes, and the function still returns void.
          if (!ops.empty()) {
            const ir::Operand& value = storage_.operands()[ops.head()];
            const ir::TypeTag tag = storage_.types()[value.type].tag;
            if (tag != ir::TypeTag::Void && tag != ir::TypeTag::Never) {
              if (const ir::TypeLayout layout =
                      storage_.layout_of(value.type, width_);
                  layout.size > 8) {
                return unsupported(span,
                                   "a return value wider than a "
                                   "register");
              }
              if (const ir::RegisterIdx reg = value.as_register();
                  reg.is_valid()) {
                const u32 vreg = vreg_of(reg, value.type);
                push(make_inst(MOp::Mov, width_of(tag), Operand{Reg::Rax},
                               Operand{VReg{vreg}}));
              } else {
                return unsupported(span, "a constant return");
              }
            }
          }
          push(make_inst(MOp::Ret, 8));
          continue;
        }
        case ir::Opcode::Unreachable: {
          push(make_inst(MOp::Ud2, 8));
          continue;
        }
        default: {
          return unsupported(span,
                             "an instruction the backend has no "
                             "lowering for");
        }
      }
    }
  }
  if (failed_) {
    return base::make_err(codegen::EmitError::Unsupported);
  }
  return base::make_ok(std::move(body_));
}

}  // namespace

base::Result<Body, codegen::EmitError> select_function(
    const ir::Storage& storage,
    const ir::SymbolTable& /*strings*/,
    std::span<const diag::Span> spans,
    const ir::Function& function,
    diag::DiagBag& bag,
    const ir::PointerWidth width) {
  Selector selector(storage, spans, function, bag, width);
  return selector.run();
}

}  // namespace codegen::x86_64
