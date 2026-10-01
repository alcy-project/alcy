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
  start: Bound<T>,
  end: Bound<T>,
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
  // steps once, honoring the language's wrapping arithmetic; only
  // hand-built intervals can hold one, since range expressions always
  // include their start. `Unbounded` has no first value, so asking
  // for one is an error rather than a guess at a minimum.
  fn first(self: Self) -> T {
    ret match self {
      Bound::Included(s) => s,
      Bound::Excluded(s) => s + 1,
      Bound::Unbounded => panic("cannot iterate a range with no start"),
    }
  }

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

  // Whether the cursor is spent after yielding `current` with this
  // stride: an inclusive end hit exactly, or a next step that would
  // pass the end. The differences are exact — a yielded value always
  // lies on the admitted side — so nothing wraps while deciding, and
  // `0..=255u8` never computes `255 + 1` at any stride.
  fn done_after(self: Self, current: T, stride: T) -> bool {
    ret match self {
      Bound::Included(e) => current == e || stride > e - current,
      Bound::Excluded(e) => stride >= e - current,
      Bound::Unbounded => false,
    }
  }
}

impl<T> Range<T> {
  // Names the cursor `for` iterates, advancing one value at a time.
  pub fn into_iter(self: Self) -> RangeIter<T> {
    ret RangeIter {
      current: self.start.first(),
      end: self.end,
      stride: 1,
      done: false,
    }
  }

  // Names a cursor advancing `stride` values at a time: `(0..<10)`
  // `.step_by(2)` yields 0, 2, 4, 6, 8. A stride must be positive;
  // zero — or, for signed ranges, a negative one — has no meaning as
  // a step, so asking for one fails rather than yielding one value
  // forever or walking downward.
  pub fn step_by(self: Self, stride: T) -> RangeIter<T> {
    if stride <= 0 {
      panic("stride must be positive")
    }
    ret RangeIter {
      current: self.start.first(),
      end: self.end,
      stride: stride,
      done: false,
    }
  }
}

impl<T> RangeIter<T> {
  // A cursor already iterates: this names itself for `for`, so both
  // `for i in 0..<n` and `for i in (0..<n).step_by(2)` read.
  pub fn into_iter(self: Self) -> RangeIter<T> {
    ret self
  }
}

impl<T> Iterator<T> for RangeIter<T> {
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
      self.current = self.current + self.stride
    }
    ret Option::Some(v)
  }
}
