// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/wasm/writer.h"

#include <bit>
#include <span>
#include <string_view>

#include "fpag/base/numeric.h"

namespace codegen::wasm {
namespace {

// The little-endian bytes of `bits`, one at a time: the format is
// little-endian, and a big-endian host would otherwise write the value
// backwards.
template <typename U, typename F>
void little_endian(BinaryWriter& out, F value) {
  const U bits = std::bit_cast<U>(value);
  for (u32 shift = 0; shift < sizeof(U) * 8; shift += 8) {
    out.byte(static_cast<u8>(bits >> shift));
  }
}

}  // namespace

void BinaryWriter::byte(u8 value) {
  bytes_.push_back(value);
}

void BinaryWriter::bytes(std::span<const u8> data) {
  bytes_.insert(bytes_.end(), data.begin(), data.end());
}

void BinaryWriter::u32_leb(u32 value) {
  while (value >= 0x80) {
    byte(static_cast<u8>(value) | 0x80);
    value >>= 7;
  }
  byte(static_cast<u8>(value));
}

void BinaryWriter::u64_leb(u64 value) {
  while (value >= 0x80) {
    byte(static_cast<u8>(value) | 0x80);
    value >>= 7;
  }
  byte(static_cast<u8>(value));
}

void BinaryWriter::i32_leb(i32 value) {
  bool more = true;
  while (more) {
    const u8 chunk = static_cast<u8>(value) & 0x7F;
    value >>= 7;
    // A signed value stops when the remaining bits are all the sign bit
    // and the sign bit of the last chunk says so.
    const bool sign = (chunk & 0x40) != 0;
    more = !((value == 0 && !sign) || (value == -1 && sign));
    byte(more ? static_cast<u8>(chunk | 0x80) : chunk);
  }
}

void BinaryWriter::i64_leb(i64 value) {
  bool more = true;
  while (more) {
    const u8 chunk = static_cast<u8>(value) & 0x7F;
    value >>= 7;
    const bool sign = (chunk & 0x40) != 0;
    more = !((value == 0 && !sign) || (value == -1 && sign));
    byte(more ? static_cast<u8>(chunk | 0x80) : chunk);
  }
}

void BinaryWriter::f32_bits(f32 value) {
  little_endian<u32>(*this, value);
}

void BinaryWriter::f64_bits(f64 value) {
  little_endian<u64>(*this, value);
}

void BinaryWriter::name(std::string_view text) {
  u32_leb(static_cast<u32>(text.size()));
  const std::span<const u8> data(reinterpret_cast<const u8*>(text.data()),
                                 text.size());
  bytes(data);
}

void write_section(BinaryWriter& out, u8 id, const BinaryWriter& payload) {
  out.byte(id);
  out.u32_leb(static_cast<u32>(payload.size()));
  out.bytes(payload.bytes());
}

}  // namespace codegen::wasm
