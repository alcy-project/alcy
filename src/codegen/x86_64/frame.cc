// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/frame.h"

#include "codegen/x86_64/minst.h"
#include "debug/dcheck.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"

namespace codegen::x86_64 {

namespace {

u64 align_up(const u64 value, const u64 align) {
  return align <= 1 ? value : (value + align - 1) / align * align;
}

// Whether a value of this width extends itself with its sign when it
// reaches a register. Only the narrow signed types do: a wider one
// already fills the register it rides in.
bool is_signed_narrow(const ir::TypeTag tag, const u64 size) {
  const bool narrow = size == 1 || size == 2;
  switch (tag) {
    case ir::TypeTag::I8:
    case ir::TypeTag::I16: return narrow;
    default: return false;
  }
}

}  // namespace

Frame layout_frame(const Body& body,
                   const ir::Storage& storage,
                   const ir::PointerWidth width) {
  Frame frame;
  frame.vreg_disp.assign(body.vreg_types.size(), 0);
  frame.vreg_size.assign(body.vreg_types.size(), 0);
  frame.vreg_signed.assign(body.vreg_types.size(), false);
  frame.vreg_pointee.assign(body.vreg_types.size(), Frame::NO_POINTEE);

  // The cursor counts bytes below `rbp`; a slot at cursor `c` holding `n`
  // bytes occupies `[-(c + n), -c)`.
  u64 cursor = 0;
  for (const Body::Alloca& alloca : body.allocas) {
    DCHECK(alloca.vreg < body.vreg_types.size());
    cursor = align_up(cursor, alloca.align) + alloca.size;
    frame.vreg_pointee[alloca.vreg] = static_cast<i32>(-cursor);
  }
  for (u32 at = 0; at < body.vreg_types.size(); ++at) {
    // A vreg an `alloca` defines is its storage's address, rematerialized
    // at every use; it needs no slot of its own.
    if (frame.vreg_pointee[at] != Frame::NO_POINTEE) {
      continue;
    }
    const ir::TypeIdx type{body.vreg_types[at]};
    const ir::TypeTag tag = storage.types()[type.idx].tag;
    const ir::TypeLayout layout = storage.layout_of(type, width);
    cursor = align_up(cursor, layout.align) + layout.size;
    frame.vreg_size[at] = static_cast<u32>(layout.size);
    frame.vreg_signed[at] = is_signed_narrow(tag, layout.size);
    frame.vreg_disp[at] = static_cast<i32>(-cursor);
  }
  // Outgoing arguments past the registers live in the bytes below the
  // locals, so a call never writes over a live slot.
  cursor += body.outgoing;
  frame.size = static_cast<u32>(align_up(cursor, 16));
  return frame;
}

}  // namespace codegen::x86_64
