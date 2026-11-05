// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: intervals as data.

// One endpoint of a `Range`. `Included(v)` and `Excluded(v)` carry the
// bound; `Unbounded` marks a side the range does not name. Range
// expressions construct these and a consumer interprets them, so the
// distinction between the two exclusion spellings survives until
// something reads it.
pub enum Bound<T> {
  Included(T),
  Excluded(T),
  Unbounded,
}

// An interval, not an iterator: the endpoints are the value. The
// language builds one from `1..<3`, `1..=3`, `a..`, `..<b`, or `..`;
// a consumer with a start and an end reads it directly, which is what
// an index does with a run (`a[1..<3]`). A present start endpoint is
// always `Included`; the end operator says whether the end is
// `Included` (`..=`) or `Excluded` (`..<`), and bare `..` names no
// end at all, so it leaves that side `Unbounded`.
pub struct Range<T> {
  pub start: Bound<T>,
  pub end: Bound<T>,
}

// A cursor over a range: `Range::into_iter` builds one, and it
// implements `Iterator` for the element type. The cursor holds the
// next value to yield, the end it stops at, the stride it advances
// by, and whether it is spent. Only integer ranges iterate; the
// endpoints of a range expression are always integers.
pub struct RangeIter<T> {
  current: T,
  end: Bound<T>,
  stride: T,
  done: bool,
}

impl<T> Bound<T> {
  // The first value a range starting at this bound yields. `Excluded`
  // steps once, honoring the language's wrapping arithmetic, so a
  // start at the width's maximum wraps instead of leaving the type;
  // only hand-built intervals can hold one, since range expressions
  // always include their start. `Unbounded` has no first value, so
  // asking for one is an error rather than a guess at a minimum.
  fn first(self: Self) -> T {
    ret match self {
      Bound::Included(s) => s,
      Bound::Excluded(s) => wrapping_add(s, 1),
      Bound::Unbounded => panic("cannot iterate a range with no start"),
    }
  }
}

impl<T: PartialOrd> Bound<T> {
  // Whether this end bound admits `current` and possibly more: every
  // value up to an inclusive end, every value below an exclusive one,
  // and everything when the end is open.
  fn admits(self: Self, current: T) -> bool {
    ret match self {
      Bound::Included(e) => current <= e,
      Bound::Excluded(e) => current < e,
      Bound::Unbounded => true,
    }
  }
}

// Whether stepping `current` by `stride` reaches `end`, counting
// `end` itself when `inclusive` is set. The step is the cursor's own
// wrapping addition, so the question is asked about the value the
// cursor would take, never about the distance between the endpoints:
// a span wider than the type's maximum - `-128i8..=127i8` - has no
// distance the element type can hold, while `0..=255u8` still never
// computes `255 + 1` at any stride. A step that wrapped lands past
// every end the type holds, and a stride is always positive.
fn step_reaches<T: PartialOrd + PartialEq>(end: T, current: T, stride: T, inclusive: bool) -> bool {
  stepped := wrapping_add(current, stride)
  if stepped < current {
    ret true
  }
  if inclusive {
    ret stepped > end
  }
  ret stepped >= end
}

impl<T: PartialOrd + PartialEq> Bound<T> {
  // Whether the cursor is spent after yielding `current` with this
  // stride: an inclusive end hit exactly, or a next step that would
  // reach or pass the end.
  fn done_after(self: Self, current: T, stride: T) -> bool {
    ret match self {
      Bound::Included(e) => step_reaches(e, current, stride, true),
      Bound::Excluded(e) => step_reaches(e, current, stride, false),
      Bound::Unbounded => false,
    }
  }
}

impl<T> Range<T> {
  // Names the cursor `for` iterates, advancing one value at a time.
  pub fn into_iter(self: Self) -> RangeIter<T> {
    ret RangeIter { current: self.start.first(), end: self.end, stride: 1, done: false, }
  }
}

impl<T: PartialOrd> Range<T> {
  // Names a cursor advancing `stride` values at a time: `(0..<10)`
  // `.step_by(2)` yields 0, 2, 4, 6, 8. A stride must be positive;
  // zero - or, for signed ranges, a negative one - has no meaning as
  // a step, so asking for one fails rather than yielding one value
  // forever or walking downward.
  pub fn step_by(self: Self, stride: T) -> RangeIter<T> {
    if stride <= 0 {
      panic("stride must be positive")
    }
    ret RangeIter { current: self.start.first(), end: self.end, stride: stride, done: false, }
  }
}

impl<T> RangeIter<T> {
  // A cursor already iterates: this names itself for `for`, so both
  // `for i in 0..<n` and `for i in (0..<n).step_by(2)` read.
  pub fn into_iter(self: Self) -> RangeIter<T> {
    ret self
  }
}

impl<T: PartialOrd + PartialEq> Iterator < T > for RangeIter<T> {
  fn next(mut self: &mut Self) -> Option<T> {
    if self.done {
      ret Option::None
    }
    if !self.end.admits(self.current) {
      self.done = true
      ret Option::None
    }
    v := self.current
    if self.end.done_after(v, self.stride) {
      self.done = true
    } else {
      stepped := wrapping_add(self.current, self.stride)
      if stepped < self.current {
        // The step would leave the type, so the value just yielded is
        // the last one and the cursor is spent. An open end has no
        // bound to test against, so it reaches this where a bounded
        // one stops before the step; its last value is yielded either
        // way, which is what lets the body stop there.
        self.done = true
      } else {
        self.current = stepped
      }
    }
    ret Option::Some(v)
  }
}

test {
  test a_range_yields_its_start_first() {
    first := find::<RangeIter<i32>, i32>((0..<5).into_iter(), (x: i32) -> true)
    assert(first.or_panic_with_message("the range yields") == 0, "the start is the first value")
  }

  test an_exclusive_end_is_not_yielded() {
    assert(how_many((0..<3).into_iter()) == 3, "the end itself stays out")
    assert(top_of((0..<3).into_iter()) == 2, "the last value is one below the end")
  }

  test an_inclusive_end_is_yielded() {
    assert(how_many((0..=3).into_iter()) == 4, "the end itself is included")
    assert(top_of((0..=3).into_iter()) == 3, "the last value is the end")
  }

  test a_range_of_one_value_yields_it() {
    assert(how_many((7..<8).into_iter()) == 1, "one value lies in the range")
    assert(top_of((7..<8).into_iter()) == 7, "and it is the start")
  }

  test an_empty_range_yields_nothing() {
    assert(how_many((3..<3).into_iter()) == 0, "no value is below the end")
    assert(yields_nothing((3..<3).into_iter()), "so there is no last value")
  }

  test an_inverted_range_yields_nothing() {
    assert(how_many((5..<2).into_iter()) == 0, "a start past the end admits nothing")
  }

  test a_spent_cursor_yields_nothing_more() {
    mut it := (0..<1).into_iter()
    assert(it.next().or_panic_with_message("the range yields") == 0, "the single value comes out")
    assert(it.next().is_none(), "the cursor is spent")
    assert(it.next().is_none(), "and stays spent")
  }

  test an_excluded_start_begins_one_past_its_value() {
    hand_built: Range<i32> := Range { start: Bound::Excluded(0), end: Bound::Included(2) }
    assert(walked(hand_built.into_iter()) == 12, "the walk starts at one")
  }

  test an_open_end_keeps_yielding() {
    mut it := (5..).into_iter()
    assert(it.next().or_panic_with_message("the range yields") == 5, "the start is the first value")
    assert(it.next().or_panic_with_message("the range yields") == 6, "the next value follows")
    assert(it.next().or_panic_with_message("the range yields") == 7, "and the one after that")
  }

  test a_cursor_iterates_as_itself() {
    cursor := (0..<3).into_iter()
    assert(how_many(cursor.into_iter()) == 3, "naming the cursor again changes nothing")
  }

  test step_by_yields_every_stride_value() {
    assert(walked((0..<10).step_by(2)) == 2468, "0, 2, 4, 6 and 8 come out in order")
  }

  test step_by_keeps_the_start() {
    assert(walked((1..<10).step_by(3)) == 147, "1, 4 and 7 come out in order")
  }

  test step_by_stops_before_passing_the_end() {
    assert(walked((0..<10).step_by(4)) == 48, "0, 4 and 8 come out and 12 does not")
  }

  test an_inclusive_stride_can_land_on_the_end() {
    assert(how_many((0..=10).step_by(5)) == 3, "0, 5 and 10 all fall in the range")
    assert(top_of((0..=10).step_by(5)) == 10, "and the end is the last of them")
  }

  test a_stride_longer_than_the_range_yields_one_value() {
    assert(how_many((5..<6).step_by(3)) == 1, "the start is the only value below the end")
  }

  test a_stride_of_one_matches_plain_iteration() {
    assert(
      how_many((0..<5).step_by(1)) == how_many((0..<5).into_iter()),
      "a stride of one steps like plain iteration",
    )
  }

  test an_unsigned_range_reaches_the_top_of_its_width() {
    assert(
      byte_count((250u8..=255u8).into_iter()) == 6,
      "the last six values of a byte are reachable",
    )
    assert(byte_top((250u8..=255u8).into_iter()) == 255u8, "and the top one is the last")
  }

  test a_for_loop_reads_the_range() {
    mut joined := 0
    for i in 3..<6 {
      joined = joined * 10 + i
    }
    assert(joined == 345, "the body sees 3, 4 and 5 in order")
  }

  // Folds the values into a decimal number, so inclusion and order both show.
  fn walked(r: RangeIter<i32>) -> i32 {
    ret fold::<RangeIter<i32>, i32, i32>(r, 0, (a: i32, x: i32) -> a * 10 + x)
  }

  fn how_many(r: RangeIter<i32>) -> usize {
    ret count::<RangeIter<i32>, i32>(r)
  }

  fn top_of(r: RangeIter<i32>) -> i32 {
    ret last::<RangeIter<i32>, i32>(r).or_panic_with_message("the range yields")
  }

  fn yields_nothing(r: RangeIter<i32>) -> bool {
    ret last::<RangeIter<i32>, i32>(r).is_none()
  }

  fn byte_count(r: RangeIter<u8>) -> usize {
    ret count::<RangeIter<u8>, u8>(r)
  }

  fn byte_top(r: RangeIter<u8>) -> u8 {
    ret last::<RangeIter<u8>, u8>(r).or_panic_with_message("the range yields")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
