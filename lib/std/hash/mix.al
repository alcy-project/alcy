// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/hash`: mixing words into a hash.
//
// xxh3 hashes bytes. A key made of words gets a cheap finalizer
// instead: integer keys mix in place, and a composite key mixes the
// digests of its fields with `mix2`. The result is a u64 suitable for
// indexing a table's slots; it is not a cryptographic digest.

use core::mem::wrapping_add;
use core::mem::wrapping_mul;

// splitmix64's finalizer. Its avalanche spreads consecutive integers
// across the whole 64-bit range, so a table keyed by an index does not
// fill one run of slots.
pub fn mix64(value: u64) -> u64 {
  mut x := wrapping_add(value, 0x9e3779b97f4a7c15u64)
  x = wrapping_mul(x ^ (x >> 30u64), 0xbf58476d1ce4e5b9u64)
  x = wrapping_mul(x ^ (x >> 27u64), 0x94d049bb133111ebu64)
  ret x ^ (x >> 31u64)
}

// Two digests become one. A composite key hashes each field and
// combines them left to right; the left digest keeps its place by
// mixing again, so swapping two equal fields changes the result.
pub fn mix2(left: u64, right: u64) -> u64 {
  ret mix64(left ^ wrapping_add(right, 0x9e3779b97f4a7c15u64))
}

#[when(testing)] {
  #[test] fn consecutive_words_spread_apart() {
    assert(mix64(1) != mix64(2), "neighbouring keys differ")
  }

  #[test] fn the_mix_is_stable() {
    assert(mix64(7) == mix64(7), "a value has one digest")
  }

  #[test] fn field_order_matters() {
    assert(mix2(1, 2) != mix2(2, 1), "a pair is ordered")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
