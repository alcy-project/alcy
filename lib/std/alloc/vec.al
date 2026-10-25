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
// the pointer. See docs/adr/0012-reborrow-on-reference-read.md.
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
  // of two above the request.
  pub fn reserve(mut self: &mut Self, extra: usize) {
    while self.cap < self.len + extra {
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

  // Exchanges the elements at `i` and `j`, both below the length.
  // Both values are read out first and written back crossed, so the
  // two slots are briefly uninitialized - which is what `read` leaves
  // behind and what `uninit_write` expects.
  pub fn swap(mut self: &mut Self, i: usize, j: usize) {
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
    mut i := 1 as usize
    while i < self.len {
      mut j := i
      while j > 0 {
        if !less(&self[j], &self[j - 1]) {
          break
        }
        self.swap(j - 1, j)
        j = j - 1
      }
      i = i + 1
    }
  }

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
    while self.len > write {
      _ := self.pop()
    }
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
  // kept prefix in place and the tail is popped, so each rejected
  // element ends once.
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
    while self.len > write {
      _ := self.pop()
    }
  }

  // Moves every element of `other` onto the end, ending `other`.
  pub fn extend(mut self: &mut Self, other: Vec<T>) {
    for value in other {
      self.push(value)
    }
  }

  // Inserts `value` at `index`, shifting the rest up. `index` may
  // equal the length, which appends.
  pub fn insert(mut self: &mut Self, index: usize, value: T) {
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
  // down and keeping their order. `index` is in bounds.
  pub fn remove(mut self: &mut Self, index: usize) -> T {
    mut i := index
    while i + 1 < self.len {
      self.swap(i, i + 1)
      i = i + 1
    }
    ret self.pop().or_panic_with_message("the vec is not empty")
  }

  // Removes and answers the element at `index`, moving the last
  // element into its place. The order of the rest is not kept, which
  // is what makes it a swap rather than a shift. `index` is in
  // bounds.
  pub fn swap_remove(mut self: &mut Self, index: usize) -> T {
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

  // Ends the elements past `n` and keeps the first `n`. A length at
  // or beyond the current one changes nothing.
  pub fn truncate(mut self: &mut Self, n: usize) {
    while self.len > n {
      self.len = self.len - 1
      unsafe { drop_in_place::<T>(uninit_assume(elem_ptr(self.buf, self.len))) }
    }
  }

  pub fn drop(mut self: Vec<T>) {
    self.clear()
    unsafe { dealloc(self.buf, self.cap) }
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

// The total order has to exist for the sort to, so the method sits in
// a block that says so: `v.sort()` on a `Vec<Plain>` names `Plain` at
// the call rather than a line in here (ADR-0028). A type that only has
// `PartialOrd` sorts through `sort_by`.
impl<T: Ord> Vec<T> {
  pub fn sort(mut self: &mut Self) {
    mut i := 1 as usize
    while i < self.len {
      mut j := i
      while j > 0 {
        if !self[j].cmp(&self[j - 1]).is_lt() {
          break
        }
        self.swap(j - 1, j)
        j = j - 1
      }
      i = i + 1
    }
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
