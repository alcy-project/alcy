// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"

namespace codegen::wasm {

// The scalar value types the binary format spells. The direct backend's
// MVP emits these and nothing else; a reference and a pointer are I32 on
// the wasm32 target it builds for.
enum class ValType : u8 {
  I32 = 0x7F,
  I64 = 0x7E,
  F32 = 0x7D,
  F64 = 0x7C,
};

// A growable byte buffer that knows the encodings the wasm binary format
// uses. It writes bytes and values; what a sequence of them means is the
// emitter's to know.
class BinaryWriter {
 public:
  void byte(u8 value);
  void bytes(std::span<const u8> data);

  // Unsigned LEB128, the encoding every index, count, and length uses.
  void u32_leb(u32 value);
  void u64_leb(u64 value);

  // Signed LEB128, the encoding the constant instructions use.
  void i32_leb(i32 value);
  void i64_leb(i64 value);

  // A float in the little-endian binary form its constant instruction
  // carries.
  void f32_bits(f32 value);
  void f64_bits(f64 value);

  // A length-prefixed UTF-8 name: a module, field, export, or local name.
  void name(std::string_view text);

  [[nodiscard]] usize size() const { return bytes_.size(); }
  [[nodiscard]] std::span<const u8> bytes() const { return bytes_; }

 private:
  std::vector<u8> bytes_;
};

// Writes a section: its id, the payload's length, and the payload. The
// length is taken from the built payload rather than patched into a
// placeholder, because an unsigned LEB128 length is not a fixed width.
void write_section(BinaryWriter& out, u8 id, const BinaryWriter& payload);

}  // namespace codegen::wasm
