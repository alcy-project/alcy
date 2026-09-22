// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "debug/dcheck.h"
#include "fpag/base/idx_range.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"

namespace ir {

// Accumulates consecutive indexes into an index range.
//
// Storage vectors require range members (e.g. Instruction::operands) to
// reference consecutive entries. Hand-rolled {head, size} pairs silently
// break when appends interleave; this guard records the head on first push
// and verifies contiguity on every push. It holds no storage itself, so
// StorageBuilder needs no changes.
//
// Convention: use SeqBuilder for ranges of two or more entries. Single entry
// ranges ({idx, 1}) and intentionally-invalid ranges in negative tests use
// braced literals directly.
template <typename IdxT>
class SeqBuilder {
 public:
  SeqBuilder() = default;

  void push(const IdxT idx) {
    if (count_ == 0) {
      head_ = idx;
    }
    DCHECK_EQ(idx.idx, head_.idx + count_);
    ++count_;
  }

  base::IdxRange<IdxT> finish() const { return {head_, count_}; }

  u32 size() const { return count_; }
  bool empty() const { return count_ == 0; }

 private:
  IdxT head_{static_cast<typename IdxT::IdxType>(0)};
  u32 count_ = 0;
};

using OperandSeq = SeqBuilder<OperandIdx>;
using InstrSeq = SeqBuilder<InstructionIdx>;
using BlockParamSeq = SeqBuilder<BlockParamIdx>;
using BlockSeq = SeqBuilder<BlockIdx>;
using TypeSeq = SeqBuilder<TypeIdx>;
using EnumVariantTypeSeq = SeqBuilder<EnumVariantTypeIdx>;

}  // namespace ir
