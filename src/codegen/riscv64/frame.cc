// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/riscv64/frame.h"

#include <algorithm>

#include "codegen/riscv64/minst.h"

namespace codegen::riscv64 {

Frame layout_frame(const Body& body) {
  Frame frame;
  frame.vreg_size = body.vreg_sizes;
  frame.vreg_disp.resize(frame.vreg_size.size(), 0);
  frame.vreg_signed = body.vreg_signed;
  u64 cursor = 0;
  for (u32 at = 0; at < frame.vreg_size.size(); ++at) {
    const u64 size = (std::max<u64>(frame.vreg_size[at], 8) + 7) & ~7ull;
    frame.vreg_disp[at] = static_cast<i32>(cursor);
    cursor += size;
  }
  // Words for the caller's registers the backend borrows; the frame
  // stays 16-byte aligned.
  cursor += SAVE_AREA_BYTES;
  frame.size = static_cast<u32>((cursor + 15) & ~15ull);
  return frame;
}

}  // namespace codegen::riscv64
