// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "fpag/base/numeric.h"
#include "xxh3.h"

namespace base {

// A 128-bit content digest: the low and high halves of `XXH3_128bits`
// over the same bytes. Recorded digests (the IR's file hashes, identity
// and footer, the cache key) are this width; a digest whose collisions
// are settled by comparing the whole stays 64 bits (`hash64`).
struct Digest128 {
  u64 low = 0;
  u64 high = 0;

  // Little-endian bytes, low half first: the order a writer emits.
  [[nodiscard]] std::array<u8, 16> bytes() const {
    std::array<u8, 16> out{};
    for (u32 i = 0; i < 8; ++i) {
      out[i] = static_cast<u8>(low >> (8 * i));
      out[8 + i] = static_cast<u8>(high >> (8 * i));
    }
    return out;
  }

  // 32 lowercase hex digits, high half first: the order `xxhsum`
  // prints, so a digest reads the same on a terminal and in a name.
  [[nodiscard]] std::string hex() const {
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::string out(32, '0');
    for (u32 i = 0; i < 32; ++i) {
      const u64 half = i < 16 ? high : low;
      out[i] = DIGITS[(half >> (4 * (15 - (i % 16)))) & 0xF];
    }
    return out;
  }

  friend bool operator==(const Digest128&, const Digest128&) = default;
};

// The project's one hash (ADR-0073). `xxh3.h` is included only here,
// so no other module names a third-party header to hash.
[[nodiscard]] inline u64 hash64(std::span<const u8> data) {
  return XXH3_64bits(data.data(), data.size());
}

[[nodiscard]] inline u64 hash64(std::string_view text) {
  return hash64(std::span<const u8>(reinterpret_cast<const u8*>(text.data()),
                                    text.size()));
}

[[nodiscard]] inline Digest128 hash128(std::span<const u8> data) {
  const XXH128_hash_t hash = XXH3_128bits(data.data(), data.size());
  return Digest128{hash.low64, hash.high64};
}

[[nodiscard]] inline Digest128 hash128(std::string_view text) {
  return hash128(std::span<const u8>(reinterpret_cast<const u8*>(text.data()),
                                     text.size()));
}

}  // namespace base
