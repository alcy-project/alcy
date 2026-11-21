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
// `PartialOrd` for the same type.
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
// `PartialEq` implementation for the same type.
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

// The pointer-width spellings are their own types: the fixed width a
// spelling shares a layout with has the same comparison, but a spelling
// declares its own implementation.
impl PartialEq for isize {
  fn eq(self: &Self, other: &Self) -> bool {
    ret *self == *other
  }
}

impl Eq for isize {
}

impl PartialEq for usize {
  fn eq(self: &Self, other: &Self) -> bool {
    ret *self == *other
  }
}

impl Eq for usize {
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

impl PartialOrd for isize {
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

impl Ord for isize {
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

impl PartialOrd for usize {
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

impl Ord for usize {
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

// A module's own tests: `#[when(testing)]` keeps them out of a build,
// and `alcy test` gathers the `#[test] fn`s into a driver that runs them.
#[when(testing)] {
  #[test] fn less_is_lt() {
    assert(Ordering::Less.is_lt(), "Less is the strictly below outcome")
  }

  #[test] fn less_is_not_eq() {
    assert(!Ordering::Less.is_eq(), "a value below is not equal")
  }

  #[test] fn less_is_not_gt() {
    assert(!Ordering::Less.is_gt(), "a value below is not above")
  }

  #[test] fn equal_is_not_lt() {
    assert(!Ordering::Equal.is_lt(), "an equal value is not below")
  }

  #[test] fn equal_is_eq() {
    assert(Ordering::Equal.is_eq(), "Equal names the equal outcome")
  }

  #[test] fn equal_is_not_gt() {
    assert(!Ordering::Equal.is_gt(), "an equal value is not above")
  }

  #[test] fn greater_is_not_lt() {
    assert(!Ordering::Greater.is_lt(), "a value above is not below")
  }

  #[test] fn greater_is_not_eq() {
    assert(!Ordering::Greater.is_eq(), "a value above is not equal")
  }

  #[test] fn greater_is_gt() {
    assert(Ordering::Greater.is_gt(), "Greater is the strictly above outcome")
  }

  #[test] fn cmp_of_a_smaller_integer_is_less() {
    a := -5
    b := 3
    assert(a.cmp(&b).is_lt(), "-5 sorts below 3")
  }

  #[test] fn cmp_of_equal_integers_is_equal() {
    a := 7
    b := 7
    assert(a.cmp(&b).is_eq(), "7 compares equal to 7")
  }

  #[test] fn cmp_of_a_larger_integer_is_greater() {
    a := 9
    b := -2
    assert(a.cmp(&b).is_gt(), "9 sorts above -2")
  }

  #[test] fn cmp_orders_signed_values_around_zero() {
    below := -1
    zero := 0
    above := 1
    assert(below.cmp(&zero).is_lt(), "a negative value sorts below zero")
    assert(zero.cmp(&above).is_lt(), "zero sorts below one")
    assert(below.cmp(&above).is_lt(), "a negative value sorts below a positive one")
  }

  #[test] fn cmp_orders_the_top_of_an_unsigned_width() {
    top := 18446744073709551615u64
    below := 18446744073709551614u64
    assert(top.cmp(&below).is_gt(), "the maximum sorts above every other width value")
    assert(below.cmp(&top).is_lt(), "and every other width value sorts below it")
  }

  #[test] fn partial_cmp_of_integers_always_has_an_order() {
    low := 1 as i8
    high := 2 as i8
    assert(
      low.partial_cmp(&high).or_panic_with_message("two i8 values compare").is_lt(),
      "1 sorts below 2",
    )
    assert(
      high.partial_cmp(&low).or_panic_with_message("two i8 values compare").is_gt(),
      "2 sorts above 1",
    )
    assert(
      high.partial_cmp(&high).or_panic_with_message("two i8 values compare").is_eq(),
      "2 is equal to 2",
    )
  }

  #[test] fn float_compares_with_itself() {
    low := 1.5f32
    high := 2.5f32
    assert(
      low.partial_cmp(&high).or_panic_with_message("two f32 values compare").is_lt(),
      "1.5 sorts below 2.5",
    )
    assert(
      high.partial_cmp(&low).or_panic_with_message("two f32 values compare").is_gt(),
      "2.5 sorts above 1.5",
    )
    assert(
      low.partial_cmp(&low).or_panic_with_message("two f32 values compare").is_eq(),
      "a float is equal to itself",
    )
  }

  #[test] fn nan_has_no_order_against_anything() {
    zero := 0.0f32
    nan := zero / zero
    assert(nan.partial_cmp(&zero).is_none(), "a NaN is incomparable with a number")
    assert(nan.partial_cmp(&nan).is_none(), "a NaN is incomparable with itself")
  }

  #[test] fn text_compares_by_its_first_differing_byte() {
    a := "abc"
    b := "abd"
    assert(a.cmp(&b).is_lt(), "c sorts below d")
    assert(b.cmp(&a).is_gt(), "and d above c")
    assert("b".cmp(&"a").is_gt(), "the first byte alone decides")
  }

  #[test] fn text_prefix_sorts_before_the_longer_text() {
    a := "ab"
    b := "abc"
    assert(a.cmp(&b).is_lt(), "the shorter text runs out of bytes first")
    assert(b.cmp(&a).is_gt(), "and the longer text sorts above it")
  }

  #[test] fn equal_text_compares_equal() {
    a := "abc"
    b := "abc"
    assert(a.cmp(&b).is_eq(), "the same bytes compare equal")
  }

  #[test] fn empty_text_sorts_before_any_other() {
    empty := ""
    a := "a"
    assert(empty.cmp(&a).is_lt(), "no bytes sorts below some bytes")
    assert(a.cmp(&empty).is_gt(), "and some bytes above none")
    assert(empty.cmp(&empty).is_eq(), "two empty texts compare equal")
  }

  #[test] fn equal_text_is_equal() {
    a := "abc"
    b := "abc"
    assert(a == b, "the same bytes are equal")
    assert(!(a != b), "and the negation agrees")
  }

  #[test] fn texts_of_different_length_are_not_equal() {
    a := "ab"
    b := "abc"
    assert(!(a == b), "a prefix is not the longer text")
    assert(a != b, "and the two are unequal")
  }

  #[test] fn texts_differing_in_one_byte_are_not_equal() {
    a := "abc"
    b := "abd"
    assert(!(a == b), "one differing byte makes the texts unequal")
    assert(a != b, "and the negation agrees")
  }

  #[test] fn empty_text_is_equal_only_to_empty_text() {
    empty := ""
    a := "a"
    assert(empty == "", "two empty texts are equal")
    assert(!(empty == a), "an empty text is not a text with a byte")
  }

  #[test] fn bool_differences_are_unequal() {
    assert(true == true, "two trues are equal")
    assert(false == false, "two falses are equal")
    assert(true != false, "true and false are unequal")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
