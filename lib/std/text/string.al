// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/text`: an owned string.

// A growable sequence of bytes, owning the buffer it reads through.
// The shape mirrors `Vec<u8>`: `buf` is uninitialized storage, `len`
// counts the initialized prefix, and `cap` counts the reserved room.
// Pushing past capacity reallocates, so a `str` view into the buffer
// is a loan of the string like any `&T` into a `Vec<T>` is.
pub struct String {
  buf: &mut MaybeUninit<u8>,
  len: usize,
  cap: usize,
}

impl String {
  pub fn new() -> String {
    ret String { buf: unsafe { alloc::<u8>(0) }, len: 0, cap: 0 }
  }

  // Reserves room for `n` bytes without changing the length. A zero
  // `n` still yields a distinct, freeable block.
  pub fn with_capacity(n: usize) -> String {
    ret String { buf: unsafe { alloc::<u8>(n) }, len: 0, cap: n }
  }

  // The number of bytes.
  pub fn len_bytes(self: &Self) -> usize {
    ret self.len
  }

  // A new string with `A`-`Z` folded to lower case; other bytes copy.
  pub fn to_ascii_lowercase(self: &Self) -> String {
    mut out := String::with_capacity(self.len)
    s := self.as_str()
    mut i := 0 as usize
    while i < str_len(s) {
      b := str_byte(s, i)
      if b >= 0x41u8 && b <= 0x5au8 {
        out.push_byte_unchecked(b + 0x20u8)
      } else {
        out.push_byte_unchecked(b)
      }
      i = i + 1
    }
    ret out
  }

  // A new string with `a`-`z` folded to upper case; other bytes copy.
  pub fn to_ascii_uppercase(self: &Self) -> String {
    mut out := String::with_capacity(self.len)
    s := self.as_str()
    mut i := 0 as usize
    while i < str_len(s) {
      b := str_byte(s, i)
      if b >= 0x61u8 && b <= 0x7au8 {
        out.push_byte_unchecked(b - 0x20u8)
      } else {
        out.push_byte_unchecked(b)
      }
      i = i + 1
    }
    ret out
  }

  // Moves the bytes of `other` on, ending `other`. The bytes are
  // copied; the source buffer ends here.
  pub fn extend(mut self: &mut Self, other: String) {
    self.push_str(other.as_str())
  }

  // A new string with every non-overlapping occurrence of `from`
  // replaced by `to`. An empty `from` would never advance, so it
  // answers a copy.
  pub fn replace(self: &Self, from: str, to: str) -> String {
    if str_len(from) == 0 {
      mut copy := String::with_capacity(self.len)
      copy.push_str(self.as_str())
      ret copy
    }
    mut out := String::new()
    mut rest := self.as_str()
    loop {
      match rest.find(from) {
        Option::Some(i) => {
          out.push_str(str_slice(rest, 0, i))
          out.push_str(to)
          rest = str_slice(rest, i + str_len(from), str_len(rest))
        }
        Option::None => {
          out.push_str(rest)
          ret out
        }
      }
    }
  }

  // A new string that repeats these bytes `n` times. A zero `n` is
  // empty.
  pub fn repeat(self: &Self, n: usize) -> String {
    mut out := String::with_capacity(self.len * n)
    mut k := 0 as usize
    while k < n {
      out.push_str(self.as_str())
      k = k + 1
    }
    ret out
  }

  // Keeps the first `n` bytes, dropping the rest. A byte has no
  // destructor, so the length is the only thing to change. `n` must
  // land on a scalar boundary; anything else answers `None` and the
  // string stays as it was.
  pub fn truncate(mut self: &mut Self, n: usize) -> Option<()> {
    if n > self.len {
      ret Option::None
    }
    if n != self.len && !self.as_str().is_char_boundary(n) {
      ret Option::None
    }
    self.len = n
    ret Option::Some(())
  }

  pub fn capacity(self: &Self) -> usize {
    ret self.cap
  }

  pub fn is_empty(self: &Self) -> bool {
    ret self.len == 0
  }

  // Internal byte append for callers that have already established
  // that the byte keeps the buffer well-formed. A public append of an
  // arbitrary byte would let a `String` hold an ill-formed sequence.
  fn push_byte_unchecked(mut self: &mut Self, b: u8) {
    if self.len == self.cap {
      self.grow()
    }
    unsafe { uninit_write(elem_ptr(self.buf, self.len), b) }
    self.len = self.len + 1
  }

  // Appends one ASCII scalar. A `char` is already known to be ASCII,
  // so the byte it holds can be written without another check.
  pub fn push_char(mut self: &mut Self, c: char) {
    self.push_byte_unchecked(c as u8)
  }

  // Appends every byte of `s`, growing once for the whole run.
  pub fn push_str(mut self: &mut Self, s: str) {
    n := str_len(s)
    self.reserve(n)
    mut i := 0 as usize
    while i < n {
      unsafe { uninit_write(elem_ptr(self.buf, self.len + i), str_byte(s, i)) }
      i = i + 1
    }
    self.len = self.len + n
  }

  // Inserts every byte of `s` at `at`, shifting the bytes after it up
  // by its length. `at` must not pass the length, and must fall on a
  // scalar boundary: a spelling that split a scalar would leave the
  // bytes ill-formed, so it panics instead.
  // Inserts every byte of `s` at `at`. `at` must fall on a scalar
  // boundary no past the length; anything else answers `None` and
  // leaves the string untouched.
  pub fn insert_str(mut self: &mut Self, at: usize, s: str) -> Option<()> {
    if at > self.len {
      ret Option::None
    }
    if !self.as_str().is_char_boundary(at) {
      ret Option::None
    }
    n := str_len(s)
    self.reserve(n)
    // The shifted run reaches past the old length, so the bytes are
    // written through the buffer rather than through `self[i]`, which
    // stops at the length.
    mut i := self.len
    while i > at {
      unsafe { uninit_write(elem_ptr(self.buf, i + n - 1), self[i - 1]) }
      i = i - 1
    }
    mut k := 0 as usize
    while k < n {
      unsafe { uninit_write(elem_ptr(self.buf, at + k), str_byte(s, k)) }
      k = k + 1
    }
    self.len = self.len + n
    ret Option::Some(())
  }

  // Reserves room for `extra` more bytes.
  pub fn reserve(mut self: &mut Self, extra: usize) {
    // The sum is formed in the wrapping form so a request past the
    // largest `usize` reaches the refusal below, rather than aborting
    // on the checked addition that computes it.
    needed := wrapping_add(self.len, extra)
    if needed < self.len {
      panic("capacity overflow")
    }
    if needed <= self.cap {
      ret
    }
    mut next := doubled_capacity(self.cap)
    if next < needed {
      next = needed
    }
    fresh := unsafe { alloc::<u8>(next) }
    unsafe { memcopy(elem_ptr(fresh, 0), elem_ref(self.buf, 0), self.len) }
    unsafe { dealloc(self.buf, self.cap) }
    self.buf = fresh
    self.cap = next
  }

  // Moves to a block twice the room, or to a first block of four. The
  // initialized prefix crosses as one byte copy.
  fn grow(mut self: &mut Self) {
    mut next := doubled_capacity(self.cap)
    if self.cap == 0 {
      next = 4
    }
    fresh := unsafe { alloc::<u8>(next) }
    unsafe { memcopy(elem_ptr(fresh, 0), elem_ref(self.buf, 0), self.len) }
    unsafe { dealloc(self.buf, self.cap) }
    self.buf = fresh
    self.cap = next
  }

  pub fn as_str(self: &Self) -> str {
    ret unsafe { str_from_parts(uninit_ref(elem_ref(self.buf, 0)), self.len) }
  }

  pub fn bytes(self: &Self) -> StrIter {
    ret self.as_str().bytes()
  }

  // A new string holding a copy of `text`.
  pub fn from(text: str) -> String {
    mut s := String::with_capacity(str_len(text))
    s.push_str(text)
    ret s
  }

  // A new string holding a copy of the bytes in `bytes`, or `None`
  // when they spell no scalars — the bytes are checked like any bytes
  // that cross into the type.
  pub fn from_slice(bytes: &[u8]) -> Option<String> {
    match from_utf8(bytes) {
      Result::Ok(text) => {
        ret Option::Some(String::from(text))
      }
      Result::Err(_) => {
        ret Option::None
      }
    }
  }

  // The byte at `at`; `at` must be below the length.
  pub fn byte(self: &Self, at: usize) -> u8 {
    ret self.as_str().byte(at)
  }

  // The view over the bytes in `[start, end)`, or `None` when a bound
  // is out of range or cuts a scalar.
  pub fn byte_slice(self: &Self, start: usize, end: usize) -> Option<str> {
    ret self.as_str().byte_slice(start, end)
  }

  pub fn as_bytes(self: &Self) -> &[u8] {
    ret unsafe { slice_from_parts(uninit_ref(elem_ref(self.buf, 0)), self.len) }
  }

  // The unused bytes after the length, for a call that fills them — a
  // reader over a descriptor, say. The caller must keep the bytes in
  // scalar order: the ones below the length stay the text they were,
  // and the ones past it are undefined until `set_len` records them,
  // so a write that crosses a scalar straddle leaves a binary blob
  // this type must not hold. The length is not changed; `set_len`
  // records how many were written.
  pub unsafe fn spare_capacity_mut(mut self: &mut Self) -> &mut [u8] {
    ret unsafe {
      slice_from_parts_mut(uninit_assume(elem_ptr(self.buf, self.len)), self.cap - self.len,)
    }
  }

  // Records `n` written bytes. `n` must not pass the capacity; the
  // bytes below it must be initialized, and must spell scalars from
  // zero — a caller whose bytes cross from somewhere outside the type
  // validates them first.
  pub unsafe fn set_len(mut self: &mut Self, n: usize) {
    self.len = n
  }

  // The bytes as an exclusive view over the current length. Growing
  // while it is live reallocates under it, so the checker reports the
  // conflict. The caller keeps the bytes in scalar order; a write that
  // rewrites the middle of the text must keep it well-formed.
  pub unsafe fn as_mut_slice(mut self: &mut Self) -> &mut [u8] {
    ret unsafe { slice_from_parts_mut(uninit_assume(elem_ptr(self.buf, 0)), self.len) }
  }

  // The byte-level predicates, so a `String` reads like the `str`
  // view it hands out.
  pub fn starts_with(self: &Self, prefix: str) -> bool {
    ret self.as_str().starts_with(prefix)
  }

  pub fn ends_with(self: &Self, suffix: str) -> bool {
    ret self.as_str().ends_with(suffix)
  }

  pub fn contains(self: &Self, needle: str) -> bool {
    ret self.as_str().contains(needle)
  }

  pub fn strip_prefix(self: &Self, prefix: str) -> Option<str> {
    ret self.as_str().strip_prefix(prefix)
  }

  pub fn trim_start_matches(self: &Self, pat: str) -> str {
    ret self.as_str().trim_start_matches(pat)
  }

  pub fn trim_end_matches(self: &Self, pat: str) -> str {
    ret self.as_str().trim_end_matches(pat)
  }

  pub fn count(self: &Self, needle: str) -> usize {
    ret self.as_str().count(needle)
  }

  // The number of scalar values, counting an ill-formed subsequence as
  // one replacement unit; see `str::count_chars`.
  pub fn count_chars(self: &Self) -> usize {
    ret self.as_str().count_chars()
  }

  pub fn lines(self: &Self) -> LinesIter {
    ret self.as_str().lines()
  }

  pub fn split_at(self: &Self, at: usize) -> Option<(str, str)> {
    ret self.as_str().split_at(at)
  }

  pub fn is_ascii(self: &Self) -> bool {
    ret self.as_str().is_ascii()
  }

  // The value read from the string's text; the capability is the
  // type's.
  pub fn parse<T: Parse>(self: &Self) -> Option<T> {
    ret T::parse(self.as_str())
  }

  pub fn split_whitespace(self: &Self) -> SplitWhitespace {
    ret self.as_str().split_whitespace()
  }

  pub fn strip_suffix(self: &Self, suffix: str) -> Option<str> {
    ret self.as_str().strip_suffix(suffix)
  }

  pub fn find(self: &Self, needle: str) -> Option<usize> {
    ret self.as_str().find(needle)
  }

  pub fn find_byte(self: &Self, b: u8) -> Option<usize> {
    ret self.as_str().find_byte(b)
  }

  pub fn rfind_byte(self: &Self, b: u8) -> Option<usize> {
    ret self.as_str().rfind_byte(b)
  }

  pub fn count_byte(self: &Self, b: u8) -> usize {
    ret self.as_str().count_byte(b)
  }

  pub fn count_matches(self: &Self, needle: str) -> usize {
    ret self.as_str().count_matches(needle)
  }

  // The trims answer a `str` view, so they borrow the buffer rather
  // than allocate.
  pub fn trim(self: &Self) -> str {
    ret self.as_str().trim()
  }

  pub fn trim_start(self: &Self) -> str {
    ret self.as_str().trim_start()
  }

  pub fn trim_end(self: &Self) -> str {
    ret self.as_str().trim_end()
  }

  pub fn split(self: &Self, delim: str) -> SplitIter {
    ret self.as_str().split(delim)
  }

  pub fn split_once(self: &Self, delim: str) -> Option<(str, str)> {
    ret self.as_str().split_once(delim)
  }

  pub fn eq_ignore_ascii_case(self: &Self, other: str) -> bool {
    ret self.as_str().eq_ignore_ascii_case(other)
  }

  pub fn rfind(self: &Self, needle: str) -> Option<usize> {
    ret self.as_str().rfind(needle)
  }

  // Forgets every byte, keeping the room already reserved.
  pub fn clear(mut self: &mut Self) {
    self.len = 0
  }

  pub fn drop(self: String) {
    unsafe { dealloc(self.buf, self.cap) }
  }
}

// `s[i]` reads a byte and `s[i] = b` writes one, panicking past the
// end the way array indexing does.
impl Index < usize, u8 > for String {
  fn index(self: &Self, i: usize) -> &u8 {
    if i >= self.len {
      panic("String index out of bounds")
    }
    ret uninit_ref(unsafe { elem_ref(self.buf, i) })
  }
}

impl PartialOrd for String {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    ret self.as_str().partial_cmp(&other.as_str())
  }
}

impl Ord for String {
  fn cmp(self: &Self, other: &Self) -> Ordering {
    ret self.as_str().cmp(&other.as_str())
  }
}

impl PartialEq for String {
  fn eq(self: &Self, other: &Self) -> bool {
    if self.len_bytes() != other.len_bytes() {
      ret false
    }
    mut i := 0 as usize
    while i < self.len_bytes() {
      if self[i] != other[i] {
        ret false
      }
      i = i + 1
    }
    ret true
  }
}

// Byte equality is reflexive, so `String` is `Eq`.
impl Eq for String {
}

// A view can build a replacement too, so a reader need not own the
// source first.
impl str {
  // A new string with `A`-`Z` folded to lower case; other bytes copy.
  pub fn to_ascii_lowercase(self: str) -> String {
    mut out := String::with_capacity(str_len(self))
    mut i := 0 as usize
    while i < str_len(self) {
      mut b := str_byte(self, i)
      if b >= 0x41u8 && b <= 0x5au8 {
        b = b + 0x20u8
      }
      out.push_byte_unchecked(b)
      i = i + 1
    }
    ret out
  }

  // A new string with `a`-`z` folded to upper case; other bytes copy.
  pub fn to_ascii_uppercase(self: str) -> String {
    mut out := String::with_capacity(str_len(self))
    mut i := 0 as usize
    while i < str_len(self) {
      mut b := str_byte(self, i)
      if b >= 0x61u8 && b <= 0x7au8 {
        b = b - 0x20u8
      }
      out.push_byte_unchecked(b)
      i = i + 1
    }
    ret out
  }

  // A new string with every non-overlapping occurrence of `from`
  // replaced by `to`. An empty pattern would never advance, so it
  // answers a copy.
  pub fn replace(self: str, from: str, to: str) -> String {
    mut out := String::new()
    if str_len(from) == 0 {
      out.push_str(self)
      ret out
    }
    mut rest := self
    loop {
      match rest.find(from) {
        Option::Some(i) => {
          out.push_str(str_slice(rest, 0, i))
          out.push_str(to)
          rest = str_slice(rest, i + str_len(from), str_len(rest))
        }
        Option::None => {
          out.push_str(rest)
          ret out
        }
      }
    }
  }
}

// A new string that joins string elements with `sep` between each.
// An empty vector answers an empty string.
pub fn join(parts: &Vec<String>, sep: str) -> String {
  mut out := String::new()
  mut i := 0 as usize
  while i < parts.len() {
    if i > 0 {
      out.push_str(sep)
    }
    out.push_str(parts.at(i).or_panic("the index is within the parts").as_str())
    i = i + 1
  }
  ret out
}

#[when(testing)] {
  // Construction: a string starts with no bytes, and reports them.

  #[test] fn new_string_is_empty() {
    s := String::new()
    assert_count(s.len_bytes(), 0, "a new string has no bytes")
    assert(s.is_empty(), "a new string is empty")
    assert_text(s.as_str(), "", "the view over no bytes is empty text")
  }

  #[test] fn string_from_text_holds_that_text() {
    s := String::from("héllo")
    assert_text(s.as_str(), "héllo", "the text survives the copy")
    assert_count(s.len_bytes(), 6, "the length counts bytes, not scalars")
    assert_count(s.count_chars(), 5, "five scalars were copied")
  }

  #[test] fn with_capacity_reserves_room_for_that_many_bytes() {
    s := String::with_capacity(16)
    assert(s.capacity() >= 16, "sixteen bytes of room were reserved")
    assert_count(s.len_bytes(), 0, "reserving room writes no bytes")
    assert(s.is_empty(), "the string is empty until something is pushed")
    assert_text(s.as_str(), "", "the view over no bytes is empty text")
  }

  #[test] fn capacity_never_falls_below_the_length() {
    mut s := String::new()
    assert(s.capacity() >= s.len_bytes(), "on an empty string")
    s.push_str("abcdefghijklmnopqrstuvwxyz")
    assert(s.capacity() >= s.len_bytes(), "after growing past the empty room")
    s.push_str("abcdefghijklmnopqrstuvwxyz")
    assert(s.capacity() >= s.len_bytes(), "after growing again")
    s.push_str("!")
    assert(s.capacity() >= s.len_bytes(), "after one more byte")
  }

  // push, push_str, and the growth path.

  #[test] fn push_str_appends_the_whole_text() {
    mut s := String::from("wid")
    s.push_str("get")
    assert_text(s.as_str(), "widget", "the whole run lands after the last byte")
    assert_count(s.len_bytes(), 6, "three bytes were added")
  }

  #[test] fn push_str_of_no_text_changes_nothing() {
    mut s := String::from("abc")
    s.push_str("")
    assert_text(s.as_str(), "abc", "the text is untouched")
    assert_count(s.len_bytes(), 3, "the length is untouched")
  }

  #[test] fn string_built_piece_by_piece_keeps_every_byte() {
    text := "abcdefghij"
    mut s := String::new()
    mut i := 0 as usize
    while i < str_len(text) {
      piece := str_slice(text, i, i + 1)
      s.push_str(piece)
      i = i + 1
    }
    assert_text(s.as_str(), text, "each byte landed after the previous one")
    assert_count(s.len_bytes(), 10, "ten bytes were added")
  }

  #[test] fn run_pushed_past_the_room_keeps_the_prefix() {
    mut s := String::from("abc")
    s.push_str("defghijklmnopqrstuvwxyz")
    assert_text(
      s.as_str(),
      "abcdefghijklmnopqrstuvwxyz",
      "the earlier bytes survive the reallocation",
    )
    assert_count(s.len_bytes(), 26, "the run was appended whole")
  }

  #[test] fn reserve_keeps_the_text_and_makes_room() {
    mut s := String::from("abc")
    s.reserve(64)
    assert(s.capacity() >= 67, "room for the length and the extra bytes")
    assert_text(s.as_str(), "abc", "reserving moves the bytes but not their text")
    assert_count(s.len_bytes(), 3, "reserving changes no length")
  }

  // insert_str at each end and in the middle.

  #[test] fn insert_str_at_the_start_prefixes_the_text() {
    mut s := String::from("world")
    s.insert_str(0, "hello ").or_panic("the offset is a boundary")
    assert_text(s.as_str(), "hello world", "the run is now the prefix")
  }

  #[test] fn insert_str_between_two_halves() {
    mut s := String::from("ac")
    s.insert_str(1, "b").or_panic("the offset is a boundary")
    assert_text(s.as_str(), "abc", "the halves stay on their sides of the run")
  }

  #[test] fn insert_str_at_the_end_appends() {
    mut s := String::from("ab")
    s.insert_str(2, "cd").or_panic("the offset is a boundary")
    assert_text(s.as_str(), "abcd", "inserting at the length appends")
  }

  #[test] fn insert_str_into_an_empty_string() {
    mut s := String::new()
    s.insert_str(0, "hi").or_panic("the offset is a boundary")
    assert_text(s.as_str(), "hi", "an empty string takes the run whole")
    assert_count(s.len_bytes(), 2, "two bytes arrived")
  }

  #[test] fn insert_str_into_a_full_buffer_shifts_past_the_room() {
    mut s := String::with_capacity(2)
    s.push_str("ab")
    s.insert_str(1, "XYZ").or_panic("the offset is a boundary")
    assert_text(s.as_str(), "aXYZb", "the tail moved past the end of the old room")
    assert_count(s.len_bytes(), 5, "the length grew by the run")
  }

  #[test] fn insert_str_of_no_text_changes_nothing() {
    mut s := String::from("abc")
    s.insert_str(1, "").or_panic("the offset is a boundary")
    assert_text(s.as_str(), "abc", "an empty run moves nothing")
    assert_count(s.len_bytes(), 3, "the length is untouched")
  }

  // truncate and clear.

  #[test] fn truncate_keeps_the_first_bytes() {
    mut s := String::from("abcdef")
    s.truncate(3).or_panic("the offset is a boundary")
    assert_text(s.as_str(), "abc", "the kept prefix is the first bytes")
    assert_count(s.len_bytes(), 3, "the length is the new one")
  }

  #[test] fn truncate_to_zero_empties_the_string() {
    mut s := String::from("abc")
    s.truncate(0).or_panic("the offset is a boundary")
    assert(s.is_empty(), "nothing is left")
    assert_text(s.as_str(), "", "the view is empty")
  }

  #[test] fn truncate_to_the_length_changes_nothing() {
    mut s := String::from("abc")
    s.truncate(3).or_panic("the offset is a boundary")
    assert_text(s.as_str(), "abc", "the length is already n")
    assert_count(s.len_bytes(), 3, "the length is unchanged")
  }

  #[test] fn truncate_past_the_length_answers_none() {
    mut s := String::from("abc")
    assert(s.truncate(99).is_none(), "a longer n is refused")
    assert_text(s.as_str(), "abc", "a longer n keeps everything")
    assert_count(s.len_bytes(), 3, "the length is unchanged")
  }

  #[test] fn byte_pushed_after_truncate_lands_at_the_new_end() {
    mut s := String::from("abcd")
    s.truncate(2).or_panic("the offset is a boundary")
    s.push_str("X")
    assert_text(s.as_str(), "abX", "the byte replaced the first dropped one")
    assert_count(s.len_bytes(), 3, "the length is the truncated one plus the push")
  }

  #[test] fn clear_empties_the_string_and_keeps_the_room() {
    mut s := String::from("a text with room")
    room := s.capacity()
    s.clear()
    assert(s.is_empty(), "clear leaves nothing")
    assert_text(s.as_str(), "", "the view is empty")
    assert(s.capacity() >= room, "the room already reserved is kept")
  }

  #[test] fn push_after_clear_starts_at_the_beginning() {
    mut s := String::from("abc")
    s.clear()
    s.push_str("xy")
    assert_text(s.as_str(), "xy", "the pushed bytes are the whole text")
    assert_count(s.len_bytes(), 2, "the cleared length is not counted")
  }

  // The views over the buffer.

  #[test] fn as_bytes_holds_every_byte_of_a_multibyte_text() {
    s := String::from("日本")
    bytes := s.as_bytes()
    assert_count(bytes.len(), 6, "the view is as long as the byte length")
    first := bytes.at(0).or_panic("the byte is within the view")
    assert_byte(*first, 0xe6u8, "the first byte of the first scalar")
    third := bytes.at(3).or_panic("the byte is within the view")
    assert_byte(*third, 0xe6u8, "the second scalar starts at byte three")
  }

  #[test] fn byte_reads_the_ends_of_the_text() {
    s := String::from("abc")
    assert_byte(s.byte(0), 0x61u8, "the first byte")
    assert_byte(s.byte(2), 0x63u8, "the last byte")
  }

  #[test] fn as_mut_slice_spans_the_length_bytes() {
    mut s := String::from("abc")
    assert_count(unsafe { s.as_mut_slice().len() }, 3, "the view stops at the length")
  }

  #[test] fn spare_capacity_mut_offers_the_room_past_the_length() {
    mut s := String::with_capacity(8)
    assert_count(unsafe { s.spare_capacity_mut().len() }, 8, "a fresh buffer is all spare")
    s.push_str("abc")
    assert_count(unsafe { s.spare_capacity_mut().len() }, 5, "the used bytes are no longer spare")
  }

  #[test] fn byte_slice_past_the_end_is_none() {
    s := String::from("abc")
    assert(s.byte_slice(1, 100).is_none(), "an end past the last byte is refused")
    match s.byte_slice(0, 2) {
      Option::Some(view) => {
        assert_text(view, "ab", "the view lands at the start")
      }
      Option::None => {
        panic("offsets 0 and 2 are boundaries")
      }
    }
  }

  // Comparison.

  #[test] fn equal_text_compares_equal() {
    a := String::from("abc")
    b := String::from("abc")
    assert(a == b, "the same bytes read as equal")
  }

  #[test] fn text_of_different_lengths_compares_unequal() {
    a := String::from("abc")
    b := String::from("abcd")
    assert(a != b, "a longer string is not equal")
  }

  #[test] fn one_differing_byte_at_the_end_compares_unequal() {
    a := String::from("abd")
    b := String::from("abc")
    assert(a != b, "the comparison reaches the last byte")
  }

  #[test] fn ordering_follows_the_bytes() {
    a := String::from("abc")
    b := String::from("abd")
    assert(a.cmp(&b).is_lt(), "the first differing byte decides")
    assert(a.cmp(&a).is_eq(), "a string compares equal to itself")
  }

  #[test] fn prefix_sorts_before_the_longer_text() {
    a := String::from("ab")
    b := String::from("abc")
    assert(a.cmp(&b).is_lt(), "the shorter text is the smaller one")
  }

  // ASCII case folding.

  #[test] fn to_ascii_lowercase_folds_uppercase_letters() {
    s := String::from("AbC")
    assert_text(s.to_ascii_lowercase().as_str(), "abc", "every uppercase letter was folded")
    assert_text(s.as_str(), "AbC", "the source is unchanged")
  }

  #[test] fn to_ascii_uppercase_folds_lowercase_letters() {
    s := String::from("aBc")
    assert_text(s.to_ascii_uppercase().as_str(), "ABC", "every lowercase letter was folded")
  }

  #[test] fn to_ascii_lowercase_leaves_a_non_ascii_byte_alone() {
    s := String::from("É")
    assert_text(
      s.to_ascii_lowercase().as_str(),
      "É",
      "a two-byte scalar has no ascii case to fold",
    )
  }

  #[test] fn to_ascii_lowercase_copies_every_other_byte() {
    s := String::from("Hi 42! Å")
    assert_text(
      s.to_ascii_lowercase().as_str(),
      "hi 42! Å",
      "digits, blanks, and punctuation copy unchanged",
    )
  }

  #[test] fn text_view_folds_case_without_an_owned_source() {
    assert_text("AbC".to_ascii_lowercase().as_str(), "abc", "a str view folds the same way")
  }

  // replace, repeat, retain, extend.

  #[test] fn replace_substitutes_every_occurrence() {
    s := String::from("a-b-c")
    assert_text(s.replace("-", "+").as_str(), "a+b+c", "each occurrence was replaced")
  }

  #[test] fn replace_does_not_overlap_occurrences() {
    s := String::from("aaa")
    assert_text(s.replace("aa", "bb").as_str(), "bba", "the first match takes both bytes")
  }

  #[test] fn replace_with_an_empty_pattern_copies_the_text() {
    s := String::from("abc")
    assert_text(s.replace("", "-").as_str(), "abc", "an empty pattern has no occurrence to replace")
  }

  #[test] fn replace_of_a_missing_pattern_copies_the_text() {
    s := String::from("abc")
    assert_text(s.replace("zz", "-").as_str(), "abc", "nothing occurred, so nothing changed")
  }

  #[test] fn replace_with_an_empty_replacement_deletes_the_occurrence() {
    s := String::from("abc")
    assert_text(s.replace("b", "").as_str(), "ac", "the neighbours close up")
  }

  #[test] fn repeat_zero_times_is_empty() {
    s := String::from("ab")
    assert(s.repeat(0).is_empty(), "no repetition leaves nothing")
  }

  #[test] fn repeat_once_copies_the_text() {
    s := String::from("ab")
    assert_text(s.repeat(1).as_str(), "ab", "one repetition is the text itself")
  }

  #[test] fn repeat_three_times_repeats_the_text() {
    s := String::from("ab")
    assert_text(s.repeat(3).as_str(), "ababab", "each repetition follows the previous one")
  }

  #[test] fn extend_appends_another_string() {
    mut s := String::from("ab")
    s.extend(String::from("cd"))
    assert_text(s.as_str(), "abcd", "the other string's bytes follow")
    assert_count(s.len_bytes(), 4, "both lengths were counted")
  }

  // join

  #[test] fn join_with_no_parts_is_empty() {
    parts := Vec::<String>::new()
    assert(join(&parts, "-").is_empty(), "no parts means no text")
  }

  #[test] fn join_puts_the_separator_between_parts() {
    mut parts := Vec::<String>::new()
    parts.push(String::from("a"))
    parts.push(String::from("bb"))
    parts.push(String::from("ccc"))
    assert_text(join(&parts, ", ").as_str(), "a, bb, ccc", "the separator sits only between parts")
  }

  #[test] fn join_of_one_part_has_no_separator() {
    mut parts := Vec::<String>::new()
    parts.push(String::from("x"))
    assert_text(join(&parts, "-").as_str(), "x", "a single part has no neighbour")
  }

  // from_slice

  #[test] fn from_slice_holds_the_given_bytes() {
    source := String::from("abc")
    match String::from_slice(source.as_bytes()) {
      Option::Some(copy) => {
        assert_text(copy.as_str(), "abc", "each byte was copied")
        assert_count(copy.len_bytes(), 3, "the length is the byte count")
      }
      Option::None => {
        panic("UTF-8 bytes are well-formed")
      }
    }
  }

  #[test] fn from_slice_of_no_bytes_is_empty() {
    source := String::new()
    match String::from_slice(source.as_bytes()) {
      Option::Some(copy) => {
        assert(copy.is_empty(), "an empty slice builds an empty string")
      }
      Option::None => {
        panic("no bytes decode")
      }
    }
  }

  // The view the accessors hand out is the whole text, in bytes.

  #[test] fn find_answers_a_byte_offset() {
    s := String::from("héllo")
    at := s.find("llo").or_panic("the needle is in the text")
    assert_count(at, 3, "the offset counts bytes")
  }

  #[test] fn find_answers_none_when_the_needle_is_absent() {
    s := String::from("abc")
    assert(s.find("zz").is_none(), "an absent needle has no offset")
  }

  #[test] fn trim_answers_a_view_without_touching_the_string() {
    s := String::from("  ab  ")
    assert_text(s.trim(), "ab", "the view drops the surrounding blanks")
    assert_count(s.len_bytes(), 6, "the string keeps its bytes")
  }

  #[test] fn strip_prefix_answers_the_remainder() {
    s := String::from("abc")
    rest := s.strip_prefix("ab").or_panic("the prefix is there")
    assert_text(rest, "c", "the remainder follows the prefix")
    assert(s.strip_prefix("zz").is_none(), "a missing prefix has no remainder")
  }

  #[test] fn split_at_answers_both_halves() {
    s := String::from("abcd")
    match s.split_at(2) {
      Option::Some((left, right)) => {
        assert_text(left, "ab", "the left half ends at the position")
        assert_text(right, "cd", "the right half starts there")
      }
      Option::None => {
        panic("offset 2 is a boundary")
      }
    }
  }

  #[test] fn eq_ignore_ascii_case_ignores_case() {
    s := String::from("AbC")
    assert(s.eq_ignore_ascii_case("aBc"), "case is the only difference")
    assert(!s.eq_ignore_ascii_case("abd"), "a differing letter is a difference")
  }

  // A string built and dropped in a loop leaves the allocator balanced.

  #[test] fn dropping_strings_built_in_a_loop_is_balanced() {
    mut i := 0 as usize
    while i < 200 {
      mut s := String::new()
      s.push_str("first ")
      s.push_str("second")
      assert_text(s.as_str(), "first second", "the buffer survived two growths")
      i = i + 1
    }
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }

  fn assert_count(actual: usize, expected: usize, what: str) {
    if actual != expected {
      panic(what)
    }
  }

  fn assert_text(actual: str, expected: str, what: str) {
    if actual != expected {
      panic(what)
    }
  }

  fn assert_byte(actual: u8, expected: u8, what: str) {
    if actual != expected {
      panic(what)
    }
  }
}
