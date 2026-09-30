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
// language builds one from `1..3`, `1..=3`, `1..<3`, `a..`, `..b`, or
// `..`; a consumer with a start and an end reads it directly, which is
// what an index does with a run (`a[1..3]`). A present start endpoint
// is included; `..=` includes the end and `..` and `..<` exclude it.
pub struct Range<T> {
  start: Bound<T>,
  end: Bound<T>,
}
