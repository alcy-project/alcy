// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: equality and order as capabilities.

// The result of a comparison, three-valued so that `Ord` can name all
// the outcomes and `PartialOrd` can wrap it in an `Option` for the
// pairs that have no order at all (a NaN, say).
pub enum Ordering {
  Less,
  Equal,
  Greater,
}

impl Ordering {
  // The three outcomes as predicates, so a caller that only needs
  // "strictly less" (an insertion sort, say) names it directly.
  pub fn is_lt(self: Self) -> bool {
    ret match self {
      Ordering::Less => true,
      Ordering::Equal => false,
      Ordering::Greater => false,
    }
  }

  pub fn is_eq(self: Self) -> bool {
    ret match self {
      Ordering::Less => false,
      Ordering::Equal => true,
      Ordering::Greater => false,
    }
  }

  pub fn is_gt(self: Self) -> bool {
    ret match self {
      Ordering::Less => false,
      Ordering::Equal => false,
      Ordering::Greater => true,
    }
  }
}

// `a < b`, `a <= b`, `a > b`, and `a >= b` reach `partial_cmp`; a
// `None` makes all four false, which is what an incomparable pair
// means. A nominal type implements this when some of its values are
// incomparable; `Ord` is for the ones where every pair compares.
pub spec PartialOrd {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering>;
}

// A total order: every pair compares, so `partial_cmp` never returns
// `None` and `cmp` names the single answer. `Ord` requires
// `PartialOrd` for the same type (ADR-0053).
pub spec Ord: PartialOrd {
  fn cmp(self: &Self, other: &Self) -> Ordering;
}

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

// The byte order is the string order: the first differing byte decides,
// and a proper prefix sorts before the longer string.
impl PartialOrd for str {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    ret Option::Some(self.cmp(other))
  }
}

impl Ord for str {
  fn cmp(self: &Self, other: &Self) -> Ordering {
    a := *self
    b := *other
    n := str_len(a)
    m := str_len(b)
    mut limit := n
    if m < limit {
      limit = m
    }
    mut i := 0 as usize
    while i < limit {
      x := str_byte(a, i)
      y := str_byte(b, i)
      if x < y {
        ret Ordering::Less
      }
      if x > y {
        ret Ordering::Greater
      }
      i = i + 1
    }
    if n < m {
      ret Ordering::Less
    }
    if n > m {
      ret Ordering::Greater
    }
    ret Ordering::Equal
  }
}

// The scalar orderings compare the pointed-to values, the way their
// equality does. Integers are totally ordered and carry `Ord`; floats
// carry `PartialOrd` alone, because a NaN compares with nothing and
// the answer is `None`.

impl PartialOrd for i8 {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    if *self < *other {
      ret Option::Some(Ordering::Less)
    }
    if *self > *other {
      ret Option::Some(Ordering::Greater)
    }
    ret Option::Some(Ordering::Equal)
  }
}

impl Ord for i8 {
  fn cmp(self: &Self, other: &Self) -> Ordering {
    if *self < *other {
      ret Ordering::Less
    }
    if *self > *other {
      ret Ordering::Greater
    }
    ret Ordering::Equal
  }
}

impl PartialOrd for i16 {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    if *self < *other {
      ret Option::Some(Ordering::Less)
    }
    if *self > *other {
      ret Option::Some(Ordering::Greater)
    }
    ret Option::Some(Ordering::Equal)
  }
}

impl Ord for i16 {
  fn cmp(self: &Self, other: &Self) -> Ordering {
    if *self < *other {
      ret Ordering::Less
    }
    if *self > *other {
      ret Ordering::Greater
    }
    ret Ordering::Equal
  }
}

impl PartialOrd for i32 {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    if *self < *other {
      ret Option::Some(Ordering::Less)
    }
    if *self > *other {
      ret Option::Some(Ordering::Greater)
    }
    ret Option::Some(Ordering::Equal)
  }
}

impl Ord for i32 {
  fn cmp(self: &Self, other: &Self) -> Ordering {
    if *self < *other {
      ret Ordering::Less
    }
    if *self > *other {
      ret Ordering::Greater
    }
    ret Ordering::Equal
  }
}

impl PartialOrd for i64 {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    if *self < *other {
      ret Option::Some(Ordering::Less)
    }
    if *self > *other {
      ret Option::Some(Ordering::Greater)
    }
    ret Option::Some(Ordering::Equal)
  }
}

impl Ord for i64 {
  fn cmp(self: &Self, other: &Self) -> Ordering {
    if *self < *other {
      ret Ordering::Less
    }
    if *self > *other {
      ret Ordering::Greater
    }
    ret Ordering::Equal
  }
}

impl PartialOrd for u8 {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    if *self < *other {
      ret Option::Some(Ordering::Less)
    }
    if *self > *other {
      ret Option::Some(Ordering::Greater)
    }
    ret Option::Some(Ordering::Equal)
  }
}

impl Ord for u8 {
  fn cmp(self: &Self, other: &Self) -> Ordering {
    if *self < *other {
      ret Ordering::Less
    }
    if *self > *other {
      ret Ordering::Greater
    }
    ret Ordering::Equal
  }
}

impl PartialOrd for u16 {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    if *self < *other {
      ret Option::Some(Ordering::Less)
    }
    if *self > *other {
      ret Option::Some(Ordering::Greater)
    }
    ret Option::Some(Ordering::Equal)
  }
}

impl Ord for u16 {
  fn cmp(self: &Self, other: &Self) -> Ordering {
    if *self < *other {
      ret Ordering::Less
    }
    if *self > *other {
      ret Ordering::Greater
    }
    ret Ordering::Equal
  }
}

impl PartialOrd for u32 {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    if *self < *other {
      ret Option::Some(Ordering::Less)
    }
    if *self > *other {
      ret Option::Some(Ordering::Greater)
    }
    ret Option::Some(Ordering::Equal)
  }
}

impl Ord for u32 {
  fn cmp(self: &Self, other: &Self) -> Ordering {
    if *self < *other {
      ret Ordering::Less
    }
    if *self > *other {
      ret Ordering::Greater
    }
    ret Ordering::Equal
  }
}

impl PartialOrd for u64 {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    if *self < *other {
      ret Option::Some(Ordering::Less)
    }
    if *self > *other {
      ret Option::Some(Ordering::Greater)
    }
    ret Option::Some(Ordering::Equal)
  }
}

impl Ord for u64 {
  fn cmp(self: &Self, other: &Self) -> Ordering {
    if *self < *other {
      ret Ordering::Less
    }
    if *self > *other {
      ret Ordering::Greater
    }
    ret Ordering::Equal
  }
}

impl PartialOrd for f32 {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    if *self < *other {
      ret Option::Some(Ordering::Less)
    }
    if *self > *other {
      ret Option::Some(Ordering::Greater)
    }
    if *self == *other {
      ret Option::Some(Ordering::Equal)
    }
    ret Option::None
  }
}

impl PartialOrd for f64 {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    if *self < *other {
      ret Option::Some(Ordering::Less)
    }
    if *self > *other {
      ret Option::Some(Ordering::Greater)
    }
    if *self == *other {
      ret Option::Some(Ordering::Equal)
    }
    ret Option::None
  }
}
