// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: equality as a capability.

// `==` reaches `eq` and `!=` its negation, so an implementation
// defines both. There is no type parameter: an equality is between two
// values of one type. Floats implement this and not `Eq`, because IEEE
// equality is not reflexive.
pub spec PartialEq {
  fn eq(self: &Self, other: &Self) -> bool;
}

// Reflexivity: `a == a` always holds. The spec declares no method of
// its own; an implementation is the promise, and it requires a
// `PartialEq` implementation for the same type (ADR-0053).
pub spec Eq: PartialEq {
}

// The scalar implementations compare the pointed-to values: `==` on
// two `&Self` would compare addresses. Integers and `bool` are
// reflexive and carry the `Eq` marker; floats carry `PartialEq` alone,
// because IEEE equality is not reflexive.
//
// `isize` and `usize` name the pointer-width fixed-width integer, so
// the implementation of that width covers them; declaring both
// spellings would be a second implementation of one type.
impl PartialEq for i8 {
  fn eq(self: &Self, other: &Self) -> bool {
    ret *self == *other
  }
}

impl Eq for i8 {
}

impl PartialEq for i16 {
  fn eq(self: &Self, other: &Self) -> bool {
    ret *self == *other
  }
}

impl Eq for i16 {
}

impl PartialEq for i32 {
  fn eq(self: &Self, other: &Self) -> bool {
    ret *self == *other
  }
}

impl Eq for i32 {
}

impl PartialEq for i64 {
  fn eq(self: &Self, other: &Self) -> bool {
    ret *self == *other
  }
}

impl Eq for i64 {
}

impl PartialEq for u8 {
  fn eq(self: &Self, other: &Self) -> bool {
    ret *self == *other
  }
}

impl Eq for u8 {
}

impl PartialEq for u16 {
  fn eq(self: &Self, other: &Self) -> bool {
    ret *self == *other
  }
}

impl Eq for u16 {
}

impl PartialEq for u32 {
  fn eq(self: &Self, other: &Self) -> bool {
    ret *self == *other
  }
}

impl Eq for u32 {
}

impl PartialEq for u64 {
  fn eq(self: &Self, other: &Self) -> bool {
    ret *self == *other
  }
}

impl Eq for u64 {
}

impl PartialEq for bool {
  fn eq(self: &Self, other: &Self) -> bool {
    ret *self == *other
  }
}

impl Eq for bool {
}

impl PartialEq for f32 {
  fn eq(self: &Self, other: &Self) -> bool {
    ret *self == *other
  }
}

impl PartialEq for f64 {
  fn eq(self: &Self, other: &Self) -> bool {
    ret *self == *other
  }
}

// `str` is a builtin primitive, not a nominal, so its comparison is the
// core suite's to declare like a scalar's. Bytes decide: equal lengths
// first, then each byte in turn.
impl PartialEq for str {
  fn eq(self: &Self, other: &Self) -> bool {
    if str_len(*self) != str_len(*other) {
      ret false
    }
    mut i := 0 as usize
    while i < str_len(*self) {
      if str_byte(*self, i) != str_byte(*other, i) {
        ret false
      }
      i = i + 1
    }
    ret true
  }
}

impl Eq for str {
}
