// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/hash`: hashing as a capability.
//
// A hash is the u64 the default algorithm computes for a value. It
// summarizes bytes without claiming anything about equality, so a
// caller that uses hashes to identify buckets asks for `Hash + Eq`:
// equal keys must hash equal for such a table to find a slot by hashing
// first and comparing only inside the probe chain.
// The digest is implementation-defined: a future hashing design may
// change the algorithm without changing this spec.

use package::mix::mix64;
use package::xxh3::xxh3_64;

pub spec Hash {
  fn hash(self: &Self) -> u64;
}

impl Hash for str {
  fn hash(self: &Self) -> u64 {
    ret xxh3_64(*self)
  }
}

// The owned string hashes through its bytes, like `str` does, so the
// two views of the same bytes produce the same digest.
impl Hash for String {
  fn hash(self: &Self) -> u64 {
    ret self.as_str().hash()
  }
}

// The integer spellings mix their bits, so two neighbouring keys do
// not land in the same run of slots. Each width casts to u64 before
// mixing; signed values keep their bit pattern through the cast.
impl Hash for i8 {
  fn hash(self: &Self) -> u64 {
    ret mix64(*self as u64)
  }
}

impl Hash for i16 {
  fn hash(self: &Self) -> u64 {
    ret mix64(*self as u64)
  }
}

impl Hash for i32 {
  fn hash(self: &Self) -> u64 {
    ret mix64(*self as u64)
  }
}

impl Hash for i64 {
  fn hash(self: &Self) -> u64 {
    ret mix64(*self as u64)
  }
}

impl Hash for u8 {
  fn hash(self: &Self) -> u64 {
    ret mix64(*self as u64)
  }
}

impl Hash for u16 {
  fn hash(self: &Self) -> u64 {
    ret mix64(*self as u64)
  }
}

impl Hash for u32 {
  fn hash(self: &Self) -> u64 {
    ret mix64(*self as u64)
  }
}

impl Hash for u64 {
  fn hash(self: &Self) -> u64 {
    ret mix64(*self)
  }
}

impl Hash for isize {
  fn hash(self: &Self) -> u64 {
    ret mix64(*self as u64)
  }
}

impl Hash for usize {
  fn hash(self: &Self) -> u64 {
    ret mix64(*self as u64)
  }
}

// A digest is useful without equality: this type deliberately omits
// `Eq`, which the separated spec allows.
struct DigestOnly {
  value: u64,
}

impl Hash for DigestOnly {
  fn hash(self: &Self) -> u64 {
    ret self.value
  }
}

#[when(testing)] {
  #[test] fn digest_needs_no_equality() {
    d := DigestOnly { value: 7 }
    assert(d.hash() == 7, "a value can be hashed without comparing equal")
  }

  #[test] fn text_hash_matches_the_xxh3_digest() {
    assert("abc".hash() == xxh3_64("abc"), "the impl uses the byte digest")
  }

  #[test] fn integer_hashes_are_distinct_and_stable() {
    assert(1u64.hash() != 2u64.hash(), "neighbouring keys differ")
    assert(1i32.hash() == 1i32.hash(), "a value has one digest")
    assert(1usize.hash() == 1usize.hash(), "the word width has a hash")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
