// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "codegen/wasm/writer.h"
#include "fpag/base/numeric.h"

namespace codegen::wasm {

// The binary format's opcode bytes, named where the emitter spells them.
// One list for every translation unit that writes instructions.
namespace op {

constexpr u8 OP_UNREACHABLE = 0x00;
constexpr u8 OP_BLOCK = 0x02;
constexpr u8 OP_LOOP = 0x03;
constexpr u8 OP_IF = 0x04;
constexpr u8 OP_ELSE = 0x05;
constexpr u8 OP_END = 0x0B;
constexpr u8 OP_BR = 0x0C;
constexpr u8 OP_BR_IF = 0x0D;
constexpr u8 OP_BR_TABLE = 0x0E;
constexpr u8 OP_RETURN = 0x0F;
constexpr u8 OP_CALL = 0x10;
constexpr u8 OP_DROP = 0x1A;
constexpr u8 OP_SELECT = 0x1B;
constexpr u8 OP_LOCAL_GET = 0x20;
constexpr u8 OP_LOCAL_SET = 0x21;
constexpr u8 OP_GLOBAL_GET = 0x23;
constexpr u8 OP_GLOBAL_SET = 0x24;
constexpr u8 OP_I32_LOAD = 0x28;
constexpr u8 OP_I32_STORE = 0x36;
constexpr u8 OP_MEMORY_SIZE = 0x3F;
constexpr u8 OP_MEMORY_GROW = 0x40;
constexpr u8 OP_I32_CONST = 0x41;
constexpr u8 OP_I64_CONST = 0x42;
constexpr u8 OP_F32_CONST = 0x43;
constexpr u8 OP_F64_CONST = 0x44;
constexpr u8 OP_I32_EQZ = 0x45;
constexpr u8 OP_I32_EQ = 0x46;
constexpr u8 OP_I32_GT_U = 0x4B;
constexpr u8 OP_I32_LE_S = 0x4C;
constexpr u8 OP_I32_GE_U = 0x4F;
constexpr u8 OP_I64_EQ = 0x51;
constexpr u8 OP_I32_ADD = 0x6A;
constexpr u8 OP_I32_SUB = 0x6B;
constexpr u8 OP_I32_MUL = 0x6C;
constexpr u8 OP_I32_AND = 0x71;
constexpr u8 OP_I32_SHL = 0x74;
constexpr u8 OP_I32_SHR_U = 0x76;

// The body of a `loop` or `block` with no parameters and no results.
constexpr u8 OP_BLOCK_VOID = 0x40;

}  // namespace op

// The one load and store the emitter writes by hand, in the runtime and
// in the instruction emitters' word loops alike.
inline void i32_load(BinaryWriter& out, u32 align_log2, u32 offset) {
  out.byte(op::OP_I32_LOAD);
  out.u32_leb(align_log2);
  out.u32_leb(offset);
}

inline void i32_store(BinaryWriter& out, u32 align_log2, u32 offset) {
  out.byte(op::OP_I32_STORE);
  out.u32_leb(align_log2);
  out.u32_leb(offset);
}

}  // namespace codegen::wasm
