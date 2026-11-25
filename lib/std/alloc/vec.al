// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/alloc`: an owned, growable sequence.

// A growable sequence of `T` values, owning the buffer it reads
// through.
//
// The buffer is uninitialized storage, so an element is readable only
// between being pushed and being popped or overwritten. `T` has to be
// movable by assignment, which is what lets a reallocation move an
// element from the old buffer to the new one.
//
// `at` takes `&Self` and hands out `&T`; the mutating accessors take
// `&mut Self`. A shared reborrow of `*self` reaches the buffer field,
// so a read-only view is a shared loan of the referent, not a copy of
// the pointer.
pub struct Vec<T> {
  buf: &mut MaybeUninit<T>,
  len: usize,
  cap: usize,
}

impl<T> Vec<T> {
  pub fn new() -> Vec<T> {
    ret Vec { buf: unsafe { alloc::<T>(0) }, len: 0, cap: 0 }
  }

  // Reserves room for `n` elements without changing the length. A zero
  // `n` still yields a distinct, freeable block.
  pub fn with_capacity(n: usize) -> Vec<T> {
    ret Vec { buf: unsafe { alloc::<T>(n) }, len: 0, cap: n }
  }

  pub fn len(self: &Self) -> usize {
    ret self.len
  }

  pub fn capacity(self: &Self) -> usize {
    ret self.cap
  }

  pub fn is_empty(self: &Self) -> bool {
    ret self.len == 0
  }

  // Appends `value`, growing the buffer when it is full.
  pub fn push(mut self: &mut Self, value: T) {
    if self.len == self.cap {
      self.grow()
    }
    unsafe { uninit_write(elem_ptr(self.buf, self.len), value) }
    self.len = self.len + 1
  }

  // Ensures room for `extra` more elements without changing the
  // length. Growth doubles, so the loop is bounded by the next power
  // of two above the request. A request past the largest `usize` is
  // refused, the way the growth step refuses a wrapped capacity: a
  // wrapped sum would name less room than the elements that follow it.
  pub fn reserve(mut self: &mut Self, extra: usize) {
    if extra > wrapping_sub(0 as usize, 1 as usize) - self.len {
      panic("capacity overflow")
    }
    while self.cap - self.len < extra {
      self.grow()
    }
  }

  // Moves to a block twice the room, or to a first block of four.
  // Elements are moved to the new buffer, so a `T` carrying a
  // destructor does not get one run for them; see docs/spec/deferred.md.
  // The move is one byte copy of the initialized prefix: every element
  // crosses once and its destructor belongs to the destination.
  fn grow(mut self: &mut Self) {
    mut next := doubled_capacity(self.cap)
    if self.cap == 0 {
      next = 4
    }
    fresh := unsafe { alloc::<T>(next) }
    unsafe { memcopy(elem_ptr(fresh, 0), elem_ref(self.buf, 0), self.len * size_of::<T>(),) }
    unsafe { dealloc(self.buf, self.cap) }
    self.buf = fresh
    self.cap = next
  }

  // The element at `index`, or `None` when `index` is past the end. A
  // shared borrow of the vector, so it stays readable while the result
  // is live.
  pub fn at(self: &Self, index: usize) -> Option<&T> {
    if index >= self.len {
      ret Option::None
    }
    ret Option::Some(uninit_ref(unsafe { elem_ref(self.buf, index) }))
  }

  // The whole buffer as a shared view. The slice borrows the
  // vector, so it stays readable while the vector is not grown.
  // The first element, or `None` when empty.
  pub fn first(self: &Self) -> Option<&T> {
    ret self.at(0)
  }

  // The last element, or `None` when empty.
  pub fn last(self: &Self) -> Option<&T> {
    if self.is_empty() {
      ret Option::None
    }
    ret self.at(self.len - 1)
  }

  pub fn as_slice(self: &Self) -> &[T] {
    ret unsafe { slice_from_parts(uninit_ref(elem_ref(self.buf, 0)), self.len) }
  }

  // The unused elements after the length, for a call that fills them
  // — a reader over a descriptor, say. The length is not changed;
  // `set_len` records how many were written.
  pub unsafe fn spare_capacity_mut(mut self: &mut Self) -> &mut [T] {
    ret unsafe {
      slice_from_parts_mut(uninit_assume(elem_ptr(self.buf, self.len)), self.cap - self.len)
    }
  }

  // Records `n` written elements. `n` must not pass the capacity; the
  // elements below it must be initialized.
  pub unsafe fn set_len(mut self: &mut Self, n: usize) {
    self.len = n
  }

  // A new vector holding a copy of `source`.
  pub fn from_slice(source: &[T]) -> Vec<T> {
    mut out := Vec::<T>::with_capacity(source.len())
    mut i := 0 as usize
    while i < source.len() {
      out.push(source[i])
      i = i + 1
    }
    ret out
  }

  // The whole buffer as an exclusive view. Growing while it is live
  // reallocates under it, so the checker reports the conflict.
  pub fn as_mut_slice(mut self: &mut Self) -> &mut [T] {
    ret unsafe { slice_from_parts_mut(uninit_assume(elem_ptr(self.buf, 0)), self.len) }
  }

  pub fn at_mut(mut self: &mut Self, index: usize) -> Option<&mut T> {
    if index >= self.len {
      ret Option::None
    }
    ret Option::Some(unsafe { uninit_assume(elem_ptr(self.buf, index)) })
  }

  // Exchanges the elements at `i` and `j`. Both indices are checked,
  // so one past the end panics the way the index operator does
  // instead of reaching outside the block the buffer owns.
  // Both values are read out first and written back crossed, so the
  // two slots are briefly uninitialized - which is what `read` leaves
  // behind and what `uninit_write` expects.
  pub fn swap(mut self: &mut Self, i: usize, j: usize) {
    if i >= self.len || j >= self.len {
      panic("Vec swap index out of bounds")
    }
    if i == j {
      ret
    }
    mut a := unsafe { slice_at_mut(self.as_mut_slice(), i) as *mut T }
    mut b := unsafe { slice_at_mut(self.as_mut_slice(), j) as *mut T }
    va := unsafe { read(a as *T) }
    vb := unsafe { read(b as *T) }
    unsafe {
      *a = vb
      *b = va
    }
  }

  // Sorts in place with a stable insertion sort: `less` decides the
  // order and equal elements keep theirs. Elements move by swapping
  // through the buffer, so a move-only `T` never copies.
  pub fn sort_by(mut self: &mut Self, less: (&T, &T) -> bool) {
    sort::stable::sort_by(self.as_mut_slice(), less)
  }

  // A left fold: `acc` starts the state and `step` carries it across
  // every element in order.
  pub fn fold<A>(self: &Self, acc: A, step: (A, &T) -> A) -> A {
    mut state := acc
    mut i := 0 as usize
    while i < self.len {
      state = step(state, self.at(i).or_panic_with_message("the index is within the vec"))
      i = i + 1
    }
    ret state
  }

  // The first element mutably, or `None` when empty.
  pub fn first_mut(mut self: &mut Self) -> Option<&mut T> {
    ret self.at_mut(0)
  }

  // The last element mutably, or `None` when empty.
  pub fn last_mut(mut self: &mut Self) -> Option<&mut T> {
    if self.is_empty() {
      ret Option::None
    }
    ret self.at_mut(self.len - 1)
  }

  // Whether the predicate accepts every element; an empty vector is
  // `true`.
  pub fn all(self: &Self, pred: (&T) -> bool) -> bool {
    mut i := 0 as usize
    while i < self.len {
      if !pred(self.at(i).or_panic_with_message("the index is within the vec")) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  // Whether the predicate accepts any element; an empty vector is
  // `false`.
  pub fn any(self: &Self, pred: (&T) -> bool) -> bool {
    mut i := 0 as usize
    while i < self.len {
      if pred(self.at(i).or_panic_with_message("the index is within the vec")) {
        ret true
      }
      i = i + 1
    }
    ret false
  }

  // The index of the last element the predicate accepts, or `None`.
  pub fn rposition(self: &Self, pred: (&T) -> bool) -> Option<usize> {
    mut i := self.len
    while i > 0 {
      i = i - 1
      if pred(self.at(i).or_panic_with_message("the index is within the vec")) {
        ret Option::Some(i)
      }
    }
    ret Option::None
  }

  // The first element the predicate accepts, or `None`.
  pub fn find(self: &Self, pred: (&T) -> bool) -> Option<&T> {
    mut i := 0 as usize
    while i < self.len {
      if pred(self.at(i).or_panic_with_message("the index is within the vec")) {
        ret self.at(i)
      }
      i = i + 1
    }
    ret Option::None
  }

  // Removes consecutive repeats the predicate says are equal, keeping
  // the first of each run and the order. The predicate sees the kept
  // element and the candidate.
  pub fn dedup_by(mut self: &mut Self, same: (&T, &T) -> bool) {
    if self.len < 2 {
      ret
    }
    mut write := 1 as usize
    mut scan := 1 as usize
    while scan < self.len {
      if !same(
        self.at(write - 1).or_panic_with_message("the index is within the vec"),
        self.at(scan).or_panic_with_message("the index is within the vec"),
      ) {
        if write != scan {
          self.swap(write, scan)
        }
        write = write + 1
      }
      scan = scan + 1
    }
    mut i := write
    while i < self.len {
      unsafe { drop_in_place::<T>(uninit_assume(elem_ptr(self.buf, i))) }
      i = i + 1
    }
    self.len = write
  }

  // The index of a value the comparator answers zero for, assuming the
  // elements are sorted by the sign it returns. `None` when none
  // matches.
  pub fn binary_search_by(self: &Self, compare: (&T) -> i32) -> Option<usize> {
    mut lo := 0 as usize
    mut hi := self.len
    while lo < hi {
      mid := lo + (hi - lo) / 2
      c := compare(self.at(mid).or_panic_with_message("the index is within the vec"))
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

  // The index of the first element the predicate accepts, or `None`.
  pub fn position(self: &Self, pred: (&T) -> bool) -> Option<usize> {
    mut i := 0 as usize
    while i < self.len {
      if pred(self.at(i).or_panic_with_message("the index is within the vec")) {
        ret Option::Some(i)
      }
      i = i + 1
    }
    ret Option::None
  }

  // Keeps the elements the predicate accepts and drops the rest, in
  // order. A write cursor follows a read cursor; the swaps leave the
  // kept prefix in place, and the rejected tail ends from first to last
  // like every other sequence.
  pub fn retain(mut self: &mut Self, keep: (&T) -> bool) {
    mut write := 0 as usize
    mut scan := 0 as usize
    while scan < self.len {
      if keep(self.at(scan).or_panic_with_message("the index is within the vec")) {
        self.swap(write, scan)
        write = write + 1
      }
      scan = scan + 1
    }
    mut i := write
    while i < self.len {
      unsafe { drop_in_place::<T>(uninit_assume(elem_ptr(self.buf, i))) }
      i = i + 1
    }
    self.len = write
  }

  // Moves every element of `other` onto the end, ending `other`.
  pub fn extend(mut self: &mut Self, other: Vec<T>) {
    for value in other {
      self.push(value)
    }
  }

  // Inserts `value` at `index`, shifting the rest up. `index` may
  // equal the length, which appends; a larger index panics.
  pub fn insert(mut self: &mut Self, index: usize, value: T) {
    if index > self.len {
      panic("Vec insert index out of bounds")
    }
    self.push(value)
    mut i := self.len - 1
    while i > index {
      self.swap(i - 1, i)
      i = i - 1
    }
  }

  // Reverses the elements in place.
  pub fn reverse(mut self: &mut Self) {
    if self.len < 2 {
      ret
    }
    mut i := 0 as usize
    mut j := self.len - 1
    while i < j {
      self.swap(i, j)
      i = i + 1
      j = j - 1
    }
  }

  // Removes and answers the element at `index`, shifting the rest
  // down and keeping their order. An index past the end panics.
  pub fn remove(mut self: &mut Self, index: usize) -> T {
    if index >= self.len {
      panic("Vec remove index out of bounds")
    }
    mut i := index
    while i + 1 < self.len {
      self.swap(i, i + 1)
      i = i + 1
    }
    ret self.pop().or_panic_with_message("the vec is not empty")
  }

  // Removes and answers the element at `index`, moving the last
  // element into its place. The order of the rest is not kept, which
  // is what makes it a swap rather than a shift. An index past the
  // end panics.
  pub fn swap_remove(mut self: &mut Self, index: usize) -> T {
    if index >= self.len {
      panic("Vec swap_remove index out of bounds")
    }
    self.swap(index, self.len - 1)
    ret self.pop().or_panic_with_message("the vec is not empty")
  }

  // Removes and returns the last element, or `None` when empty.
  pub fn pop(mut self: &mut Self) -> Option<T> {
    if self.len == 0 {
      ret Option::None
    }
    self.len = self.len - 1
    ret Option::Some(unsafe { *uninit_assume(elem_ptr(self.buf, self.len)) })
  }

  // Forgets every element, keeping the room already reserved.
  pub fn clear(mut self: &mut Self) {
    mut i := 0 as usize
    while i < self.len {
      unsafe { drop_in_place::<T>(uninit_assume(elem_ptr(self.buf, i))) }
      i = i + 1
    }
    self.len = 0
  }

  // Ends the elements past `n` and keeps the first `n`, from first to
  // last like every other sequence. A length at or beyond the
  // current one changes nothing.
  pub fn truncate(mut self: &mut Self, n: usize) {
    if self.len <= n {
      ret
    }
    mut i := n
    while i < self.len {
      unsafe { drop_in_place::<T>(uninit_assume(elem_ptr(self.buf, i))) }
      i = i + 1
    }
    self.len = n
  }

  pub fn drop(mut self: Vec<T>) {
    self.clear()
    unsafe { dealloc(self.buf, self.cap) }
  }

  // A new vector of `n` elements, each built by `make` from its index.
  // The maker is called once per position, from zero upward, so an
  // element type that is not `Copy` can be built in place without a
  // source value to replicate.
  pub fn from_fn(n: usize, make: (usize) -> T) -> Vec<T> {
    mut out := Vec::<T>::with_capacity(n)
    mut i := 0 as usize
    while i < n {
      out.push(make(i))
      i = i + 1
    }
    ret out
  }

  // A new vector of each element passed through `f`. The result's
  // element type is the closure's return type, which the call reads
  // from the closure argument's own signature.
  pub fn map<U>(self: &Self, f: (&T) -> U) -> Vec<U> {
    mut out := Vec::<U>::new()
    out.reserve(self.len)
    mut i := 0 as usize
    while i < self.len {
      out.push(f(self.at(i).or_panic_with_message("the index is within the vec")))
      i = i + 1
    }
    ret out
  }

  // Consumes the vector; the cursor owns what is left.
  pub fn into_iter(self: Vec<T>) -> VecIntoIter<T> {
    ret VecIntoIter { v: self, pos: 0 }
  }
}

// Equality has to exist for `contains` to compare elements, so the
// method sits in a block that says so: `v.contains(&x)` on a
// `Vec<Plain>` names `Plain` at the call rather than a line in here
impl<T: Copy + Zeroable> Vec<T> {
  // A new vector of `n` zeroed elements. The bounds make the fill
  // safe: a `Copy` value is its bytes, and a `Zeroable` type spells a
  // valid value when those bytes are all zero.
  pub fn zeros(n: usize) -> Vec<T> {
    mut out := Vec::<T>::with_capacity(n)
    unsafe { memset(elem_ptr(out.buf, 0), 0u8, n) }
    unsafe { out.set_len(n) }
    ret out
  }
}

impl<T: PartialEq> Vec<T> {
  // Whether some element equals `value`, compared the way `==`
  // compares two elements.
  pub fn contains(self: &Self, value: &T) -> bool {
    mut i := 0 as usize
    while i < self.len {
      if *self.at(i).or_panic_with_message("the index is within the vec") == *value {
        ret true
      }
      i = i + 1
    }
    ret false
  }
}

// The total order has to exist for the sort to, so the method sits in
// a block that says so: `v.sort()` on a `Vec<Plain>` names `Plain` at
// the call rather than a line in here. A type that only has
// `PartialOrd` sorts through `sort_by`.
impl<T: Ord> Vec<T> {
  pub fn sort(mut self: &mut Self) {
    sort::stable::sort(self.as_mut_slice())
  }
}

// A consuming cursor over a `Vec`. Elements leave one at a time from
// the front; the cursor's own destructor ends the ones still there and
// releases the buffer, so a loop that stops early leaks nothing.
pub struct VecIntoIter<T> {
  v: Vec<T>,
  pos: usize,
}

impl<T> VecIntoIter<T> {
  pub fn into_iter(self: VecIntoIter<T>) -> VecIntoIter<T> {
    ret self
  }

  pub fn drop(self: VecIntoIter<T>) {
    mut i := self.pos
    while i < self.v.len() {
      unsafe { drop_in_place::<T>(uninit_assume(elem_ptr(self.v.buf, i))) }
      i = i + 1
    }
    unsafe { dealloc(self.v.buf, self.v.cap) }
  }
}

impl<T> Iterator < T > for VecIntoIter<T> {
  fn next(mut self: &mut Self) -> Option<T> {
    if self.pos >= self.v.len() {
      ret Option::None
    }
    v := unsafe { *uninit_assume(elem_ptr(self.v.buf, self.pos)) }
    self.pos = self.pos + 1
    ret Option::Some(v)
  }
}

// A shared cursor over a `Vec`. Elements are handed out by reference,
// and the vector's loan held in the cursor is what keeps them valid.
pub struct VecIter<T> {
  buf: &MaybeUninit<T>,
  len: usize,
  pos: usize,
}

impl<T> Vec<T> {
  // Borrows the vector for as long as the cursor lives.
  pub fn iter(self: &Self) -> VecIter<T> {
    ret VecIter { buf: self.buf, len: self.len, pos: 0 }
  }
}

impl<T> VecIter<T> {
  pub fn into_iter(self: VecIter<T>) -> VecIter<T> {
    ret self
  }
}

impl<T> Iterator < &T > for VecIter<T> {
  fn next(mut self: &mut Self) -> Option<&T> {
    if self.pos >= self.len {
      ret Option::None
    }
    v := uninit_ref(unsafe { elem_ref(self.buf, self.pos) })
    self.pos = self.pos + 1
    ret Option::Some(v)
  }
}

// An exclusive cursor over a `Vec`. Each element is handed out by
// exclusive reference; the vector's loan held in the cursor is what
// keeps them valid.
pub struct VecIterMut<T> {
  buf: &mut MaybeUninit<T>,
  len: usize,
  pos: usize,
}

impl<T> Vec<T> {
  // Borrows the vector exclusively for as long as the cursor lives.
  pub fn iter_mut(mut self: &mut Self) -> VecIterMut<T> {
    ret VecIterMut { buf: self.buf, len: self.len, pos: 0 }
  }
}

impl<T> VecIterMut<T> {
  pub fn into_iter(self: VecIterMut<T>) -> VecIterMut<T> {
    ret self
  }
}

impl<T> Iterator < &mut T > for VecIterMut<T> {
  fn next(mut self: &mut Self) -> Option<&mut T> {
    if self.pos >= self.len {
      ret Option::None
    }
    v := unsafe { uninit_assume(elem_ptr(self.buf, self.pos)) }
    self.pos = self.pos + 1
    ret Option::Some(v)
  }
}

// `a[i]` reads through `at` and panics on a miss, which is what array
// indexing does.
impl<T> Index < usize, T > for Vec<T> {
  fn index(self: &Self, i: usize) -> &T {
    ret self.at(i).or_panic_with_message("index out of bounds")
  }
}

impl<T> IndexMut < usize, T > for Vec<T> {
  fn index_mut(mut self: &mut Self, i: usize) -> &mut T {
    ret self.at_mut(i).or_panic_with_message("index out of bounds")
  }
}

// `==` compares lengths and then elements. Whether `T` compares is
// checked where an instantiation happens, which is what bounds will
// state up front once they exist.
impl<T: PartialEq> PartialEq for Vec<T> {
  fn eq(self: &Self, other: &Self) -> bool {
    if self.len() != other.len() {
      ret false
    }
    mut i := 0 as usize
    while i < self.len() {
      if self[i] != other[i] {
        ret false
      }
      i = i + 1
    }
    ret true
  }
}

#[when(testing)] {
  #[test] fn new_vec_holds_nothing_and_reserves_nothing() {
    v := Vec::<i32>::new()
    assert(v.is_empty(), "a vec that was never pushed into holds nothing")
    assert(v.len() == 0, "a vec that was never pushed into is of length zero")
    assert(v.capacity() == 0, "a vec that was never pushed into has no room yet")
  }

  #[test] fn with_capacity_reserves_room_without_a_length() {
    v := Vec::<i32>::with_capacity(8)
    assert(v.capacity() == 8, "the room asked for is the room the vec reports")
    assert(v.is_empty(), "reserving room adds no element")
  }

  #[test] fn push_puts_each_value_behind_the_last() {
    mut v := Vec::<i32>::new()
    v.push(1)
    v.push(2)
    assert(v.len() == 2, "one element per push")
    assert(!v.is_empty(), "a pushed vec is no longer empty")
    assert(value(&v, 0) == 1, "the first push takes the first index")
    assert(value(&v, 1) == 2, "the second push follows it")
  }

  #[test] fn first_push_takes_a_block_of_four() {
    mut v := Vec::<i32>::new()
    v.push(1)
    assert(v.capacity() == 4, "an empty vec reads its first block as four elements")
  }

  #[test] fn grown_vec_keeps_every_element_in_order() {
    mut v := Vec::<i32>::new()
    mut i := 0 as i32
    while i < 5 {
      v.push(i)
      i = i + 1
    }
    assert(v.len() == 5, "five pushes are five elements")
    assert(v.capacity() == 8, "the full block of four grows to eight")
    assert(value(&v, 0) == 0, "the element pushed before the growth is still first")
    assert(value(&v, 4) == 4, "the last push is still last")
  }

  #[test] fn reserve_raises_the_room_without_a_length() {
    mut v := Vec::<i32>::new()
    v.push(1)
    v.reserve(10)
    assert(v.capacity() >= 11, "the room covers the element held and the ten asked for")
    assert(v.len() == 1, "reserving room adds no element")
    assert(value(&v, 0) == 1, "the element held stays where it was")
  }

  #[test] fn at_answers_none_past_the_end() {
    vals := [1i32, 2i32]
    v := vec_of(vals.as_slice())
    assert(value(&v, 0) == 1, "the first index names the first element")
    assert(v.at(2).is_none(), "one past the last element is not an element")
  }

  #[test] fn first_and_last_of_an_empty_vec_are_none() {
    v := Vec::<i32>::new()
    assert(v.first().is_none(), "an empty vec has no first element")
    assert(v.last().is_none(), "an empty vec has no last element")
  }

  #[test] fn first_and_last_name_the_ends() {
    vals := [1i32, 2i32, 3i32]
    v := vec_of(vals.as_slice())
    assert(
      *v.first().or_panic_with_message("the vec is not empty") == 1,
      "the first element comes first",
    )
    assert(
      *v.last().or_panic_with_message("the vec is not empty") == 3,
      "the last element comes last",
    )
  }

  #[test] fn at_mut_writes_through_to_the_element() {
    vals := [1i32, 2i32]
    mut v := vec_of(vals.as_slice())
    slot := v.at_mut(1).or_panic_with_message("the index is inside the vec")
    *slot = 9
    assert(value(&v, 1) == 9, "the write lands in the element at the index")
    assert(value(&v, 0) == 1, "the other element is untouched")
  }

  #[test] fn last_mut_names_the_last_element() {
    vals := [3i32, 4i32]
    mut v := vec_of(vals.as_slice())
    slot := v.last_mut().or_panic_with_message("the vec is not empty")
    *slot = 5
    assert(value(&v, 1) == 5, "the write lands in the last element")
  }

  #[test] fn pop_hands_back_the_last_element_then_nothing() {
    vals := [1i32, 2i32]
    mut v := vec_of(vals.as_slice())
    assert(
      v.pop().or_panic_with_message("the vec is not empty") == 2,
      "the last element comes out first",
    )
    assert(v.len() == 1, "a pop shortens the vec")
    assert(
      v.pop().or_panic_with_message("the vec is not empty") == 1,
      "the element before it comes out next",
    )
    assert(v.pop().is_none(), "an emptied vec has nothing to hand back")
  }

  #[test] fn insert_at_the_length_appends() {
    vals := [1i32, 2i32]
    mut v := vec_of(vals.as_slice())
    v.insert(2, 3)
    assert(v.len() == 3, "an insert adds one element")
    assert(value(&v, 2) == 3, "an index equal to the length puts the value last")
  }

  #[test] fn insert_at_zero_shifts_the_rest_up() {
    vals := [1i32, 2i32]
    mut v := vec_of(vals.as_slice())
    v.insert(0, 9)
    assert(value(&v, 0) == 9, "the inserted value takes the index")
    assert(value(&v, 1) == 1, "the element that was first follows it")
    assert(value(&v, 2) == 2, "the rest keeps its order")
  }

  #[test] fn remove_hands_back_the_element_and_closes_the_gap() {
    vals := [1i32, 2i32, 3i32]
    mut v := vec_of(vals.as_slice())
    assert(v.remove(1) == 2, "the element at the index comes out")
    assert(v.len() == 2, "a removal shortens the vec")
    assert(value(&v, 1) == 3, "the elements after the gap shift down in order")
  }

  #[test] fn swap_remove_fills_the_gap_with_the_last_element() {
    vals := [1i32, 2i32, 3i32]
    mut v := vec_of(vals.as_slice())
    assert(v.swap_remove(0) == 1, "the element at the index comes out")
    assert(v.len() == 2, "a swap removal shortens the vec")
    assert(value(&v, 0) == 3, "the last element moves into the gap")
    assert(value(&v, 1) == 2, "the element between them stays where it was")
  }

  #[test] fn swap_exchanges_two_elements() {
    vals := [1i32, 2i32, 3i32]
    mut v := vec_of(vals.as_slice())
    v.swap(0, 2)
    assert(value(&v, 0) == 3, "the last element is now first")
    assert(value(&v, 1) == 2, "the element between them did not move")
    assert(value(&v, 2) == 1, "the first element is now last")
  }

  #[test] fn swap_of_one_index_with_itself_changes_nothing() {
    vals := [1i32, 2i32]
    mut v := vec_of(vals.as_slice())
    v.swap(1, 1)
    assert(value(&v, 0) == 1, "the element outside the swap is untouched")
    assert(value(&v, 1) == 2, "the element the swap names twice is untouched")
  }

  #[test] fn reverse_turns_the_order_around() {
    vals := [1i32, 2i32, 3i32]
    mut v := vec_of(vals.as_slice())
    v.reverse()
    assert(value(&v, 0) == 3, "the last element is now first")
    assert(value(&v, 1) == 2, "the middle element stays in the middle")
    assert(value(&v, 2) == 1, "the first element is now last")
  }

  #[test] fn clear_empties_the_vec_and_keeps_the_room() {
    mut v := Vec::<i32>::with_capacity(4)
    v.push(1)
    v.push(2)
    v.clear()
    assert(v.is_empty(), "clearing leaves no element")
    assert(v.capacity() == 4, "the room reserved before the clear is kept")
    v.push(3)
    assert(value(&v, 0) == 3, "a cleared vec takes a new element")
  }

  #[test] fn truncate_keeps_the_first_count() {
    vals := [1i32, 2i32, 3i32, 4i32]
    mut v := vec_of(vals.as_slice())
    v.truncate(2)
    assert(v.len() == 2, "the length stops at the count kept")
    assert(value(&v, 0) == 1, "the kept elements are the first ones")
    assert(value(&v, 1) == 2, "in the order they arrived in")
  }

  #[test] fn truncate_past_the_length_changes_nothing() {
    vals := [1i32, 2i32]
    mut v := vec_of(vals.as_slice())
    v.truncate(5)
    assert(v.len() == 2, "a count past the length keeps every element")
    assert(value(&v, 1) == 2, "and leaves them where they were")
  }

  #[test] fn retain_keeps_the_accepted_elements_in_order() {
    vals := [1i32, 2i32, 3i32, 4i32, 5i32]
    mut v := vec_of(vals.as_slice())
    v.retain((x: &i32) -> *x % 2 == 1)
    assert(v.len() == 3, "the rejected elements leave")
    assert(value(&v, 0) == 1, "the first accepted element is still first")
    assert(value(&v, 1) == 3, "the accepted elements keep their order")
    assert(value(&v, 2) == 5, "and the last one is still last")
  }

  #[test] fn dedup_by_removes_each_run_of_repeats() {
    vals := [1i32, 1i32, 2i32, 2i32, 1i32]
    mut v := vec_of(vals.as_slice())
    v.dedup_by((a: &i32, b: &i32) -> *a == *b)
    assert(v.len() == 3, "each run of repeats keeps one element")
    assert(value(&v, 0) == 1, "the first run keeps its value")
    assert(value(&v, 1) == 2, "the second run keeps its value")
    assert(value(&v, 2) == 1, "a repeat that is not consecutive is kept")
  }

  #[test] fn extend_moves_every_element_of_the_other_vec() {
    vals2 := [2i32, 3i32]
    vals := [1i32]
    mut v := vec_of(vals.as_slice())
    v.extend(vec_of(vals2.as_slice()))
    assert(v.len() == 3, "the other vec's elements are added")
    assert(value(&v, 0) == 1, "the element held stays first")
    assert(value(&v, 1) == 2, "the added elements follow in order")
    assert(value(&v, 2) == 3, "and end the vec")
  }

  #[test] fn zeros_fills_every_element_with_zero() {
    v := Vec::<i32>::zeros(4)
    assert(v.len() == 4, "one zero per requested element")
    assert(value(&v, 0) == 0, "the first element is zero")
    assert(value(&v, 3) == 0, "and so is the last")
  }

  #[test] fn zeros_of_zero_indices_is_empty() {
    v := Vec::<i32>::zeros(0)
    assert(v.is_empty(), "no element is filled")
  }

  #[test] fn zeros_accepts_bool_and_char() {
    flags := Vec::<bool>::zeros(2)
    assert(!flags[0], "all-zero bytes spell false")
    chars := Vec::<char>::zeros(2)
    assert(chars[1] == '\0', "all-zero bytes spell the NUL scalar")
  }

  #[test] fn from_fn_builds_one_element_per_index() {
    v := Vec::<i32>::from_fn(4, (i: usize) -> i as i32 * i as i32)
    assert(v.len() == 4, "one element per index")
    assert(value(&v, 0) == 0, "the first index builds the first element")
    assert(value(&v, 3) == 9, "the last index builds the last element")
  }

  #[test] fn from_fn_of_zero_indices_is_empty() {
    v := Vec::<i32>::from_fn(0, (i: usize) -> 7)
    assert(v.is_empty(), "no index means no element is built")
  }

  #[test] fn map_takes_each_element_through_the_closure() {
    vals := [1i32, 2i32]
    v := vec_of(vals.as_slice())
    doubled := v.map((x: &i32) -> *x * 2)
    assert(doubled.len() == 2, "one element per element")
    assert(value(&doubled, 0) == 2, "the first element went through the closure")
    assert(value(&doubled, 1) == 4, "and so did the second")
    assert(value(&v, 0) == 1, "the vec the closure read is unchanged")
  }

  #[test] fn fold_carries_the_state_across_every_element() {
    vals := [1i32, 2i32, 3i32]
    v := vec_of(vals.as_slice())
    total := v.fold(0 as i32, (acc: i32, x: &i32) -> acc + *x)
    assert(total == 6, "the state crosses every element in order")
    empty_total := Vec::<i32>::new().fold(10 as i32, (acc: i32, x: &i32) -> acc + *x)
    assert(empty_total == 10, "an empty vec answers the state it was given")
  }

  #[test] fn all_and_any_read_the_predicate_over_the_elements() {
    vals := [2i32, 4i32]
    v := vec_of(vals.as_slice())
    assert(v.all((x: &i32) -> *x % 2 == 0), "every element passes the predicate")
    assert(v.any((x: &i32) -> *x == 4), "one element passes the predicate")
    vals2 := [2i32, 3i32]
    w := vec_of(vals2.as_slice())
    assert(!(w.all((x: &i32) -> *x % 2 == 0)), "one element that fails makes all false")
    assert(w.any((x: &i32) -> *x % 2 == 1), "one element that passes makes any true")
  }

  #[test] fn all_of_an_empty_vec_is_true_and_any_is_false() {
    v := Vec::<i32>::new()
    assert(v.all((x: &i32) -> *x > 0), "no element fails the predicate")
    assert(!(v.any((x: &i32) -> *x > 0)), "no element passes the predicate")
  }

  #[test] fn position_answers_the_first_index_the_predicate_accepts() {
    vals := [1i32, 2i32, 2i32]
    v := vec_of(vals.as_slice())
    found := v.position((x: &i32) -> *x > 1).or_panic_with_message("an element is above one")
    assert(found == 1, "the first index the predicate accepts is the answer")
    assert(v.position((x: &i32) -> *x > 2).is_none(), "a predicate nothing accepts finds no index")
  }

  #[test] fn rposition_answers_the_last_index_the_predicate_accepts() {
    vals := [1i32, 2i32, 2i32]
    v := vec_of(vals.as_slice())
    found := v.rposition((x: &i32) -> *x > 1).or_panic_with_message("an element is above one")
    assert(found == 2, "the search runs from the end of the vec")
    assert(v.rposition((x: &i32) -> *x > 2).is_none(), "a predicate nothing accepts finds no index")
  }

  #[test] fn find_answers_the_first_element_the_predicate_accepts() {
    vals := [1i32, 2i32, 3i32]
    v := vec_of(vals.as_slice())
    assert(
      *v.find((x: &i32) -> *x > 1).or_panic_with_message("an element is above one") == 2,
      "the first element the predicate accepts comes back",
    )
    assert(v.find((x: &i32) -> *x > 3).is_none(), "a predicate nothing accepts finds nothing")
  }

  #[test] fn binary_search_by_answers_the_index_of_a_match() {
    vals := [1i32, 3i32, 5i32]
    v := vec_of(vals.as_slice())
    found := v.binary_search_by((x: &i32) -> *x - 3).or_panic_with_message("three sits in the vec")
    assert(found == 1, "the compare that answers zero names the index")
    assert(
      v.binary_search_by((x: &i32) -> *x - 4).is_none(),
      "a value the vec does not hold has no index",
    )
  }

  #[test] fn contains_compares_the_elements_for_equality() {
    vals := [1i32, 2i32]
    v := vec_of(vals.as_slice())
    assert(v.contains(&2), "an element the vec holds is found")
    assert(!(v.contains(&9)), "an element the vec does not hold is not found")
  }

  #[test] fn sort_orders_the_elements() {
    vals := [3i32, 1i32, 2i32]
    mut v := vec_of(vals.as_slice())
    v.sort()
    assert(value(&v, 0) == 1, "the smallest element ends up first")
    assert(value(&v, 1) == 2, "the elements in between follow")
    assert(value(&v, 2) == 3, "the largest element ends up last")
  }

  #[test] fn sort_by_takes_the_order_from_the_predicate() {
    vals := [2i32, 3i32, 1i32]
    mut v := vec_of(vals.as_slice())
    v.sort_by((a: &i32, b: &i32) -> *a > *b)
    assert(value(&v, 0) == 3, "the order the predicate names puts the largest first")
    assert(value(&v, 1) == 2, "the elements in between follow")
    assert(value(&v, 2) == 1, "and the smallest last")
  }

  #[test] fn equality_compares_the_length_then_the_elements() {
    vals := [1i32, 2i32]
    a := vec_of(vals.as_slice())
    mut wide := Vec::<i32>::with_capacity(8)
    wide.push(1)
    wide.push(2)
    assert(a == wide, "the same elements in the same order are equal, whatever room they hold")
    vals2 := [1i32]
    assert(!(a == vec_of(vals2.as_slice())), "a vec of another length is not equal")
    vals3 := [1i32, 3i32]
    assert(!(a == vec_of(vals3.as_slice())), "an element that differs makes the vecs unequal")
  }

  #[test] fn as_slice_is_a_view_of_every_element() {
    vals := [1i32, 2i32]
    v := vec_of(vals.as_slice())
    view := v.as_slice()
    assert(view.len() == 2, "the view has one element per element")
    assert(
      *view.at(0).or_panic_with_message("the view holds an index 0") == 1,
      "the view reads the first element",
    )
    assert(
      *view.at(1).or_panic_with_message("the view holds an index 1") == 2,
      "the view reads the last element",
    )
  }

  #[test] fn as_mut_slice_writes_through_to_the_elements() {
    vals := [1i32, 2i32]
    mut v := vec_of(vals.as_slice())
    view := v.as_mut_slice()
    slot := unsafe { slice_at_mut(view, 1) }
    *slot = 8
    assert(value(&v, 0) == 1, "the element outside the write is untouched")
    assert(value(&v, 1) == 8, "the write lands in the element at the index")
  }

  #[test] fn iter_yields_every_element_by_reference_in_order() {
    vals := [1i32, 2i32, 3i32]
    v := vec_of(vals.as_slice())
    mut total := 0i32
    for x in v.iter() {
      total = total * 10 + *x
    }
    assert(total == 123, "the cursor walks the elements once, in order")
  }

  #[test] fn iter_mut_writes_through_every_element() {
    vals := [1i32, 2i32]
    mut v := vec_of(vals.as_slice())
    for x in v.iter_mut() {
      *x = *x + 1
    }
    assert(value(&v, 0) == 2, "the first element was written")
    assert(value(&v, 1) == 3, "the last element was written")
  }

  #[test] fn into_iter_yields_every_element_then_nothing() {
    vals := [1i32, 2i32]
    mut cursor := vec_of(vals.as_slice()).into_iter()
    assert(
      cursor.next().or_panic_with_message("the cursor holds an index 0") == 1,
      "the first element comes out",
    )
    assert(
      cursor.next().or_panic_with_message("the cursor holds an index 1") == 2,
      "then the second",
    )
    assert(cursor.next().is_none(), "one past the last element hands out nothing")
  }

  #[test] fn index_operators_reach_the_elements() {
    vals := [1i32, 2i32]
    mut v := vec_of(vals.as_slice())
    assert(v[0] + v[1] == 3, "the read operator names the element at the index")
    v[1] = 5
    assert(v[1] == 5, "the write operator lands in the element at the index")
    assert(value(&v, 0) == 1, "the other element is untouched")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }

  fn value(v: &Vec<i32>, i: usize) -> i32 {
    ret *v.at(i).or_panic_with_message("the index is inside the vec")
  }

  fn vec_of(values: &[i32]) -> Vec<i32> {
    mut out := Vec::<i32>::new()
    for x in values.iter() {
      out.push(*x)
    }
    ret out
  }
}
