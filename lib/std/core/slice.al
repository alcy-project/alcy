// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: borrowed slices over elements.

// A slice is a view into a run of `T`: a pointer plus a length, with
// no ownership. Slices only occur behind a reference (`&[T]`), the
// way `str` is a view into bytes; `str` stays its own type rather
// than spelling as `&[u8]`.
pub intrinsic fn slice_len<T>(s: &[T]) -> usize;

pub unsafe intrinsic fn slice_from_parts<T>(ptr: &T, len: usize) -> &[T];

pub unsafe intrinsic fn slice_from_parts_mut<T>(ptr: &mut T, len: usize) -> &mut [T];

// The element at `i`; in bounds exactly when `i < slice_len(s)`.
pub unsafe intrinsic fn slice_at<T>(s: &[T], i: usize) -> &T;

// The mutable element at `i`; in bounds exactly when `i < slice_len(s)`.
pub unsafe intrinsic fn slice_at_mut<T>(s: &mut [T], i: usize) -> &mut T;

impl<T>[T] {
  // The element count.
  pub fn len(self: &Self) -> usize {
    ret slice_len(self)
  }

  pub fn is_empty(self: &Self) -> bool {
    ret slice_len(self) == 0
  }

  // The element at `index`, or `None` when past the end.
  pub fn at(self: &Self, index: usize) -> Option<&T> {
    if index >= slice_len(self) {
      ret Option::None
    }
    ret Option::Some(unsafe { slice_at(self, index) })
  }

  // The first element, or `None` when empty.
  pub fn first(self: &Self) -> Option<&T> {
    if slice_len(self) == 0 {
      ret Option::None
    }
    ret Option::Some(unsafe { slice_at(self, 0) })
  }

  // The last element, or `None` when empty.
  pub fn last(self: &Self) -> Option<&T> {
    n := slice_len(self)
    if n == 0 {
      ret Option::None
    }
    ret Option::Some(unsafe { slice_at(self, n - 1) })
  }

  // The index of the first element the predicate accepts, or `None`.
  pub fn position(self: &Self, pred: (&T) -> bool) -> Option<usize> {
    mut i := 0 as usize
    while i < slice_len(self) {
      if pred(self.at(i).or_panic_with_message("the index is within the slice")) {
        ret Option::Some(i)
      }
      i = i + 1
    }
    ret Option::None
  }

  // The first element the predicate accepts, or `None`.
  pub fn find(self: &Self, pred: (&T) -> bool) -> Option<&T> {
    mut i := 0 as usize
    while i < slice_len(self) {
      if pred(self.at(i).or_panic_with_message("the index is within the slice")) {
        ret self.at(i)
      }
      i = i + 1
    }
    ret Option::None
  }

  // The last index the predicate accepts, or `None`.
  pub fn rposition(self: &Self, pred: (&T) -> bool) -> Option<usize> {
    mut i := slice_len(self)
    while i > 0 {
      i = i - 1
      if pred(self.at(i).or_panic_with_message("the index is within the slice")) {
        ret Option::Some(i)
      }
    }
    ret Option::None
  }

  // Whether every element is at or before the one after it, by `less`.
  // An empty slice and a one-element slice are sorted.
  pub fn is_sorted_by(self: &Self, less: (&T, &T) -> bool) -> bool {
    mut i := 1 as usize
    while i < slice_len(self) {
      if less(
        self.at(i).or_panic_with_message("the index is within the slice"),
        self.at(i - 1).or_panic_with_message("the index is within the slice"),
      ) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  // The index `compare` answers zero for, or `None`. The slice must be
  // ordered the way `compare` orders it.
  pub fn binary_search_by(self: &Self, compare: (&T) -> i32) -> Option<usize> {
    mut lo := 0 as usize
    mut hi := slice_len(self)
    while lo < hi {
      mid := lo + (hi - lo) / 2
      c := compare(self.at(mid).or_panic_with_message("the index is within the slice"))
      if c == 0 {
        ret Option::Some(mid)
      }
      if c < 0 {
        lo = mid + 1
      } else {
        hi = mid
      }
    }
    ret Option::None
  }

  // The first element mutably, or `None` when empty.
  pub fn first_mut(mut self: &mut Self) -> Option<&mut T> {
    if slice_len(self) == 0 {
      ret Option::None
    }
    ret Option::Some(unsafe { slice_at_mut(self, 0) })
  }

  // The last element mutably, or `None` when empty.
  pub fn last_mut(mut self: &mut Self) -> Option<&mut T> {
    n := slice_len(self)
    if n == 0 {
      ret Option::None
    }
    ret Option::Some(unsafe { slice_at_mut(self, n - 1) })
  }

  // The views before and after `at`. `at` may equal the length, which
  // leaves the tail empty; a larger boundary panics.
  pub fn split_at(self: &Self, at: usize) -> (&[T], &[T]) {
    n := slice_len(self)
    if at > n {
      panic("slice split out of bounds")
    }
    if at == n {
      ret (self, unsafe { slice_from_parts(slice_at(self, 0), 0) })
    }
    head := unsafe { slice_from_parts(slice_at(self, 0), at) }
    tail := unsafe { slice_from_parts(slice_at(self, at), n - at) }
    ret (head, tail)
  }

  // The first element and the rest, or `None` when empty.
  pub fn split_first(self: &Self) -> Option<(&T, &[T])> {
    if slice_len(self) == 0 {
      ret Option::None
    }
    ret Option::Some(
      (self.at(0).or_panic_with_message("the slice is not empty"), self.split_at(1).1)
    )
  }

  // The last element and the front, or `None` when empty.
  pub fn split_last(self: &Self) -> Option<(&T, &[T])> {
    n := slice_len(self)
    if n == 0 {
      ret Option::None
    }
    ret Option::Some(
      (self.at(n - 1).or_panic_with_message("the slice is not empty"), self.split_at(n - 1).0)
    )
  }

  // A shared cursor over the elements; the slice's loan is what keeps
  // them valid.
  pub fn iter(self: &Self) -> SliceIter<T> {
    ret SliceIter { s: self, pos: 0 }
  }

  // An exclusive cursor over the elements.
  pub fn iter_mut(mut self: &mut Self) -> SliceIterMut<T> {
    ret SliceIterMut { s: self, pos: 0 }
  }
}

// The comparisons walk elements, so they sit in a block that says
// what they need: `s.contains(&x)` on a `[Plain]` names `Plain` at the
// call rather than a line in here.
impl<T: PartialEq>[T] {
  // Whether some element equals `value`, compared the way `==`
  // compares two elements.
  pub fn contains(self: &Self, value: &T) -> bool {
    mut i := 0 as usize
    while i < slice_len(self) {
      if *self.at(i).or_panic_with_message("the index is within the slice") == *value {
        ret true
      }
      i = i + 1
    }
    ret false
  }

  // Whether the view begins with `prefix`; an empty prefix matches.
  pub fn starts_with(self: &Self, prefix: &[T]) -> bool {
    n := slice_len(prefix)
    if n > slice_len(self) {
      ret false
    }
    mut i := 0 as usize
    while i < n {
      if *self.at(i).or_panic_with_message("the index is within the slice") != *prefix.at(i).or_panic_with_message(
        "the index is within the slice"
      ) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  // Whether the view ends with `suffix`; an empty suffix matches.
  pub fn ends_with(self: &Self, suffix: &[T]) -> bool {
    n := slice_len(suffix)
    m := slice_len(self)
    if n > m {
      ret false
    }
    mut i := 0 as usize
    while i < n {
      if *self.at(m - n + i).or_panic_with_message("the index is within the slice") != *suffix.at(i).or_panic_with_message(
        "the index is within the slice"
      ) {
        ret false
      }
      i = i + 1
    }
    ret true
  }
}

// A shared cursor over a slice's elements.
pub struct SliceIter<T> {
  s: &[T],
  pos: usize,
}

impl<T> SliceIter<T> {
  pub fn into_iter(self: SliceIter<T>) -> SliceIter<T> {
    ret self
  }
}

impl<T> Iterator < &T > for SliceIter<T> {
  fn next(mut self: &mut Self) -> Option<&T> {
    if self.pos >= slice_len(self.s) {
      ret Option::None
    }
    v := unsafe { slice_at(self.s, self.pos) }
    self.pos = self.pos + 1
    ret Option::Some(v)
  }
}

impl<T, N: usize>[T; N] {
  // The element count; `N` is the array's own.
  pub fn len(self: &Self) -> usize {
    ret N
  }

  pub fn is_empty(self: &Self) -> bool {
    ret N == 0
  }

  // The array as a shared view. The base is the array's own address,
  // read without the index check, so an array of no elements has a
  // view too: element zero is where the array starts whatever `N` is.
  pub fn as_slice(self: &Self) -> &[T] {
    ret unsafe { slice_from_parts(&*(self as *T), N) }
  }

  // The array as a mutable view.
  pub fn as_mut_slice(mut self: &mut Self) -> &mut [T] {
    ret unsafe { slice_from_parts_mut(&mut *(self as *mut T), N) }
  }

  pub fn iter(self: &Self) -> SliceIter<T> {
    ret self.as_slice().iter()
  }

  pub fn iter_mut(mut self: &mut Self) -> SliceIterMut<T> {
    ret SliceIterMut { s: self.as_mut_slice(), pos: 0 }
  }

  // Consumes the array; the cursor owns what is left.
  pub fn into_iter(self: [T; N]) -> ArrayIntoIter<T, N> {
    ret ArrayIntoIter { a: self, pos: 0 }
  }
}

// A consuming cursor over an array. Each element leaves by value; the
// cursor's destructor ends the ones still there, so a loop that stops
// early leaks nothing.
pub struct ArrayIntoIter<T, N: usize> {
  a: [T; N],
  pos: usize,
}

impl<T, N: usize> ArrayIntoIter<T, N> {
  pub fn into_iter(self: ArrayIntoIter < T, N >) -> ArrayIntoIter<T, N> {
    ret self
  }

  pub fn drop(mut self: ArrayIntoIter < T, N >) {
    mut i := self.pos
    while i < N {
      unsafe { drop_in_place(slice_at_mut(self.a.as_mut_slice(), i)) }
      i = i + 1
    }
  }
}

impl<T, N: usize> Iterator < T > for ArrayIntoIter<T, N> {
  fn next(mut self: &mut Self) -> Option<T> {
    if self.pos >= N {
      ret Option::None
    }
    at := unsafe { slice_at(self.a.as_slice(), self.pos) } as *T
    v := unsafe { read(at) }
    self.pos = self.pos + 1
    ret Option::Some(v)
  }
}

// An exclusive cursor over a slice's elements.
pub struct SliceIterMut<T> {
  s: &mut [T],
  pos: usize,
}

impl<T> SliceIterMut<T> {
  pub fn into_iter(self: SliceIterMut<T>) -> SliceIterMut<T> {
    ret self
  }
}

impl<T> Iterator < &mut T > for SliceIterMut<T> {
  fn next(mut self: &mut Self) -> Option<&mut T> {
    if self.pos >= slice_len(self.s) {
      ret Option::None
    }
    v := unsafe { slice_at_mut(self.s, self.pos) }
    self.pos = self.pos + 1
    ret Option::Some(v)
  }
}

// A module's own tests: 'alcy test' splices the block into a driver and
// runs each entry as a process of its own.
test {
  test len_counts_the_elements() {
    a := [1i32, 2i32, 3i32]
    assert(a.len() == 3, "an array counts its own N")
    assert(a.as_slice().len() == 3, "a view counts the same elements")
  }

  test a_view_of_no_elements_is_empty() {
    a := [1i32, 2i32, 3i32]
    empty := a.as_slice().split_at(0).0
    assert(empty.is_empty(), "nothing is left before the first element")
    assert(empty.len() == 0, "the count of nothing is zero")
  }

  test at_reads_both_ends() {
    a := [1i32, 2i32, 3i32]
    s := a.as_slice()
    assert(*s.at(0).or_panic_with_message("the first index is in bounds") == 1, "the first element")
    assert(*s.at(2).or_panic_with_message("the last index is in bounds") == 3, "the last element")
  }

  test at_past_the_last_index_is_none() {
    a := [1i32, 2i32, 3i32]
    assert(a.as_slice().at(3).is_none(), "one past the last index reads nothing")
  }

  test first_and_last_answer_the_ends() {
    a := [1i32, 2i32, 3i32]
    s := a.as_slice()
    assert(*s.first().or_panic_with_message("the view is not empty") == 1, "the first element")
    assert(*s.last().or_panic_with_message("the view is not empty") == 3, "the last element")
  }

  test the_ends_of_an_empty_view_are_none() {
    a := [1i32, 2i32, 3i32]
    empty := a.as_slice().split_at(0).0
    assert(empty.first().is_none(), "an empty view has no first element")
    assert(empty.last().is_none(), "an empty view has no last element")
  }

  test position_reports_the_first_match() {
    a := [1i32, 2i32, 1i32]
    s := a.as_slice()
    assert(
      s.position((x: &i32) -> *x == 1).or_panic_with_message("1 is in the view") == 0,
      "the run holds two matches and the first one wins",
    )
    assert(
      s.position((x: &i32) -> *x > 1).or_panic_with_message("2 is in the view") == 1,
      "a predicate no earlier element accepts reports that later index",
    )
  }

  test position_of_an_absent_element_is_none() {
    a := [1i32, 2i32, 3i32]
    assert(
      a.as_slice().position((x: &i32) -> *x == 9).is_none(),
      "a predicate no element accepts reports none",
    )
  }

  test find_answers_the_first_match() {
    a := [1i32, 2i32, 1i32]
    v := a.as_slice().find((x: &i32) -> *x > 1).or_panic_with_message("2 is in the view")
    assert(*v == 2, "the first element past the threshold")
  }

  test find_of_an_absent_element_is_none() {
    a := [1i32, 2i32, 3i32]
    assert(a.as_slice().find((x: &i32) -> *x == 9).is_none(), "no element satisfies the predicate")
  }

  test rposition_reports_the_last_match() {
    a := [1i32, 2i32, 1i32]
    assert(
      a.as_slice().rposition((x: &i32) -> *x == 1).or_panic_with_message("1 is in the view") == 2,
      "the search runs backwards",
    )
  }

  test rposition_of_an_absent_element_is_none() {
    a := [1i32, 2i32, 3i32]
    assert(
      a.as_slice().rposition((x: &i32) -> *x == 9).is_none(),
      "a predicate no element accepts reports none",
    )
  }

  test is_sorted_by_accepts_an_ordered_view() {
    a := [1i32, 2i32, 2i32, 3i32]
    assert(
      a.as_slice().is_sorted_by((x: &i32, y: &i32) -> *x < *y),
      "equal neighbours do not break the order",
    )
  }

  test is_sorted_by_rejects_a_descent() {
    a := [1i32, 3i32, 2i32]
    assert(
      !(a.as_slice().is_sorted_by((x: &i32, y: &i32) -> *x < *y)),
      "a later element before its predecessor is a descent",
    )
  }

  test is_sorted_by_accepts_a_view_of_at_most_one_element() {
    a := [1i32, 2i32, 3i32]
    s := a.as_slice()
    assert(
      s.split_at(0).0.is_sorted_by((x: &i32, y: &i32) -> *x < *y),
      "an empty view has no pair to break",
    )
    assert(
      s.split_at(1).0.is_sorted_by((x: &i32, y: &i32) -> *x < *y),
      "a single element has no pair either",
    )
  }

  test binary_search_by_finds_both_ends() {
    a := [1i32, 2i32, 3i32, 4i32, 5i32]
    s := a.as_slice()
    assert(
      s.binary_search_by((x: &i32) -> *x - 1).or_panic_with_message("1 is in the view") == 0,
      "the first element",
    )
    assert(
      s.binary_search_by((x: &i32) -> *x - 5).or_panic_with_message("5 is in the view") == 4,
      "the last element",
    )
  }

  test binary_search_by_of_an_absent_element_is_none() {
    a := [1i32, 2i32, 3i32]
    assert(
      a.as_slice().binary_search_by((x: &i32) -> *x - 9).is_none(),
      "a value the view lacks is found nowhere",
    )
  }

  test binary_search_by_on_an_empty_view_is_none() {
    a := [1i32, 2i32, 3i32]
    assert(
      a.as_slice().split_at(0).0.binary_search_by((x: &i32) -> *x).is_none(),
      "there is no midpoint to compare",
    )
  }

  test split_at_reports_the_elements_either_side() {
    a := [1i32, 2i32, 3i32]
    pair := a.as_slice().split_at(2)
    assert(pair.0.len() == 2, "two elements sit before the split")
    assert(
      *pair.0.at(1).or_panic_with_message("the head holds an index 1") == 2,
      "the head ends at the split",
    )
    assert(pair.1.len() == 1, "one element sits after it")
    assert(
      *pair.1.at(0).or_panic_with_message("the tail holds an index 0") == 3,
      "the tail starts at the split",
    )
  }

  test split_at_zero_puts_the_whole_view_in_the_tail() {
    a := [1i32, 2i32, 3i32]
    pair := a.as_slice().split_at(0)
    assert(pair.0.is_empty(), "nothing sits before the first element")
    assert(pair.1.len() == 3, "all of it sits after")
  }

  test split_at_the_end_puts_the_whole_view_in_the_head() {
    a := [1i32, 2i32, 3i32]
    pair := a.as_slice().split_at(3)
    assert(pair.0.len() == 3, "all of it sits before")
    assert(pair.1.is_empty(), "nothing sits after the last element")
  }

  test split_first_reports_the_head_and_the_rest() {
    a := [1i32, 2i32, 3i32]
    pair := a.as_slice().split_first().or_panic_with_message("the view is not empty")
    assert(*pair.0 == 1, "the element that left")
    assert(pair.1.len() == 2, "the elements that stayed")
  }

  test split_last_reports_the_tail_and_the_front() {
    a := [1i32, 2i32, 3i32]
    pair := a.as_slice().split_last().or_panic_with_message("the view is not empty")
    assert(*pair.0 == 3, "the element that left")
    assert(pair.1.len() == 2, "the elements that stayed")
  }

  test split_first_of_an_empty_view_is_none() {
    a := [1i32, 2i32, 3i32]
    assert(a.as_slice().split_at(0).0.split_first().is_none(), "there is no head to take")
  }

  test split_last_of_an_empty_view_is_none() {
    a := [1i32, 2i32, 3i32]
    assert(a.as_slice().split_at(0).0.split_last().is_none(), "there is no tail to take")
  }

  test iter_yields_each_element_in_order() {
    a := [1i32, 2i32, 3i32]
    mut total := 0i32
    for x in a.as_slice().iter() {
      total = total * 10 + *x
    }
    assert(total == 123, "the cursor walks the elements once, in order")
  }

  test the_cursor_stops_after_the_last_element() {
    a := [1i32, 2i32]
    mut it := a.as_slice().iter()
    first := *it.next().or_panic_with_message("the view holds an index 0")
    second := *it.next().or_panic_with_message("the view holds an index 1")
    assert(first == 1, "the cursor answered the first element")
    assert(second == 2, "and then the second")
    assert(it.next().is_none(), "one past the last element reads nothing")
  }

  test iter_mut_writes_through_every_element() {
    mut a := [1i32, 2i32, 3i32]
    for x in a.iter_mut() {
      *x = *x * 2
    }
    assert(a[0] == 2, "the first element was written")
    assert(a[2] == 6, "the last element was written")
  }

  test into_iter_yields_every_element_by_value() {
    a := [1i32, 2i32, 3i32]
    mut total := 0i32
    for x in a.into_iter() {
      total = total + x
    }
    assert(total == 6, "the consuming cursor hands each element out once")
  }

  test contains_finds_an_element_in_the_middle() {
    a := [1i32, 2i32, 3i32]
    assert(a.as_slice().contains(&2i32), "the value sits between the ends")
  }

  test contains_of_an_absent_element_is_false() {
    a := [1i32, 2i32, 3i32]
    assert(!(a.as_slice().contains(&9i32)), "the value appears nowhere")
  }

  test starts_with_a_matching_prefix() {
    a := [1i32, 2i32, 3i32]
    b := [1i32, 2i32]
    assert(a.as_slice().starts_with(b.as_slice()), "the view opens with both prefix elements")
  }

  test starts_with_of_a_longer_prefix_is_false() {
    a := [1i32, 2i32]
    b := [1i32, 2i32, 3i32]
    assert(!(a.as_slice().starts_with(b.as_slice())), "a prefix longer than the view cannot match")
  }

  test starts_with_rejects_a_prefix_that_differs() {
    a := [1i32, 2i32, 3i32]
    b := [1i32, 9i32]
    assert(!(a.as_slice().starts_with(b.as_slice())), "the second element already differs")
  }

  test the_empty_prefix_matches_every_view() {
    a := [1i32, 2i32, 3i32]
    empty := a.as_slice().split_at(0).0
    assert(a.as_slice().starts_with(empty), "a view begins with nothing")
  }

  test ends_with_a_matching_suffix() {
    a := [1i32, 2i32, 3i32]
    b := [2i32, 3i32]
    assert(a.as_slice().ends_with(b.as_slice()), "the view closes with both suffix elements")
  }

  test ends_with_of_a_longer_suffix_is_false() {
    a := [2i32, 3i32]
    b := [1i32, 2i32, 3i32]
    assert(!(a.as_slice().ends_with(b.as_slice())), "a suffix longer than the view cannot match")
  }

  test ends_with_rejects_a_suffix_that_differs() {
    a := [1i32, 2i32, 3i32]
    b := [9i32, 3i32]
    assert(!(a.as_slice().ends_with(b.as_slice())), "the earlier element already differs")
  }

  test the_empty_suffix_matches_every_view() {
    a := [1i32, 2i32, 3i32]
    empty := a.as_slice().split_at(0).0
    assert(a.as_slice().ends_with(empty), "a view ends with nothing")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
