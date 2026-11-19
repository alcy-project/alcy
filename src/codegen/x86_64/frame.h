// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "codegen/x86_64/minst.h"
#include "ir/common.h"
#include "ir/storage.h"

namespace codegen::x86_64 {

// The frame one body occupies: where every vreg's slot sits below `rbp`,
// and how many bytes the entry has to reserve. Layout runs before
// allocation and is a pure function of the body and its types, so a
// change here is visible in the object and in nothing else.
struct Frame {
  // Total bytes below `rbp`, a multiple of 16 so a call's stack stays
  // aligned.
  u32 size = 0;
  // One entry per vreg: the slot's displacement from `rbp`, always
  // negative.
  std::vector<i32> vreg_disp;
  // One entry per vreg: the bytes the slot holds. A slot wider than a
  // register moves as bytes, which is what an aggregate is.
  std::vector<u32> vreg_size;
  // For a vreg an `alloca` defines, the displacement of the storage it
  // addresses. -1 for a vreg that holds an ordinary value.
  std::vector<i32> vreg_pointee;
};

// Lays the frame out from the body's slot requests. Every `alloca` gets
// its own storage whether or not the block that names it runs more than
// once: a slot in a loop body is reserved once, at the entry, or the
// stack walks down one slot per turn.
[[nodiscard]] Frame layout_frame(const Body& body,
                                 const ir::Storage& storage,
                                 ir::PointerWidth width);

}  // namespace codegen::x86_64
