// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: the `str` view over bytes.

pub intrinsic fn str_len(s: str) -> usize;

pub intrinsic fn str_byte(s: str, i: usize) -> u8;

pub intrinsic fn str_slice(s: str, start: usize, end: usize) -> str;

pub unsafe intrinsic fn str_from_parts(ptr: &u8, len: usize) -> str;

// The byte length of the scalar at `at`, or zero where the bytes do
// not form one: a stray continuation, a lead the alphabet does not
// grant, an overlong form, a surrogate, or one past the last scalar.
fn utf8_scalar_length(bytes: &[u8], at: usize) -> usize {
  mut want := 0 as usize
  mut lo := 0x80u8
  mut hi := 0xBFu8
  mut i := at
  b := bytes[i]
  n := bytes.len()
  if b < 0x80u8 {
    ret 1
  }
  if b >= 0xC2u8 && b <= 0xDFu8 {
    want = 2
  } else if b == 0xE0u8 {
    want = 3
    lo = 0xA0u8
  } else if b >= 0xE1u8 && b <= 0xECu8 {
    want = 3
  } else if b == 0xEDu8 {
    want = 3
    hi = 0x9Fu8
  } else if b >= 0xEEu8 && b <= 0xEFu8 {
    want = 3
  } else if b == 0xF0u8 {
    want = 4
    lo = 0x90u8
  } else if b >= 0xF1u8 && b <= 0xF3u8 {
    want = 4
  } else if b == 0xF4u8 {
    want = 4
    hi = 0x8Fu8
  }
  if want == 0 {
    ret 0
  }
  mut k := 1 as usize
  while k < want {
    if i + k >= n {
      ret 0
    }
    c := bytes[i + k]
    mut fits := c >= 0x80u8 && c <= 0xBFu8
    if k == 1 {
      fits = c >= lo && c <= hi
    }
    if !fits {
      ret 0
    }
    k = k + 1
  }
  ret want
}

// The offset of the first byte that breaks a scalar, or `None` when
// the bytes are well-formed.
fn utf8_scan(bytes: &[u8]) -> Option<Utf8Error> {
  mut i := 0 as usize
  while i < bytes.len() {
    seq := utf8_scalar_length(bytes, i)
    if seq == 0 {
      ret Option::Some(Utf8Error { valid_up_to: i })
    }
    i = i + seq
  }
  ret Option::None
}

// The failure a UTF-8 check reports: the offset of the first byte
// that did not decode.
pub struct Utf8Error {
  pub valid_up_to: usize,
}

// Whether the bytes are well-formed UTF-8: every slot decodes, no
// surrogate is encoded, and no sequence is overlong or truncated.
pub fn is_utf8(bytes: &[u8]) -> bool {
  ret utf8_scan(bytes).is_none()
}

// The view over `bytes`, or the first offset that did not decode.
pub fn from_utf8(bytes: &[u8]) -> Result<str, Utf8Error> {
  match utf8_scan(bytes) {
    Option::Some(e) => {
      ret Result::Err(e)
    }
    Option::None => {}
  }
  n := bytes.len()
  if n == 0 {
    ret Result::Ok("")
  }
  ret Result::Ok(unsafe { str_from_parts(slice_at(bytes, 0), n) })
}

// An ASCII letter folded to lower case; other bytes pass through.
fn ascii_lower(b: u8) -> u8 {
  if b >= 0x41u8 && b <= 0x5au8 {
    ret b + 0x20u8
  }
  ret b
}

// The ASCII whitespace `trim` strips: space, tab, newline, vertical
// tab, form feed, and carriage return.
fn is_ascii_space(b: u8) -> bool {
  if b == 0x20u8 {
    ret true
  }
  if b == 0x09u8 {
    ret true
  }
  if b == 0x0au8 {
    ret true
  }
  if b == 0x0bu8 {
    ret true
  }
  if b == 0x0cu8 {
    ret true
  }
  if b == 0x0du8 {
    ret true
  }
  ret false
}

impl str {
  // The number of bytes.
  pub fn len_bytes(self: &Self) -> usize {
    ret str_len(*self)
  }

  pub fn is_empty(self: &Self) -> bool {
    ret str_len(*self) == 0
  }

  // The view over the bytes in `[start, end)`. The two positions must
  // land on scalar boundaries and stay within the view; anything else
  // answers `None` rather than slicing into the middle of a scalar.
  pub fn byte_slice(self: &Self, start: usize, end: usize) -> Option<str> {
    m := str_len(*self)
    if start > m || end > m || start > end {
      ret Option::None
    }
    if start != end && !self.is_char_boundary(start) {
      ret Option::None
    }
    if start != end && !self.is_char_boundary(end) {
      ret Option::None
    }
    ret Option::Some(str_slice(*self, start, end))
  }

  // The byte at `at`; in bounds exactly when `at < self.len_bytes()`.
  pub fn byte(self: &Self, at: usize) -> u8 {
    ret str_byte(*self, at)
  }

  // Whether `at` falls on a scalar boundary: a position past the end,
  // or one whose byte does not continue a scalar that began earlier.
  // A run of continuation bytes with no lead byte in front of it is not
  // well-formed, so each of its positions answers as a boundary: the
  // predicate reads one byte, and `utf8::valid` is what judges the
  // whole text.
  pub fn is_char_boundary(self: &Self, at: usize) -> bool {
    if at >= str_len(*self) {
      ret true
    }
    b := str_byte(*self, at)
    ret b < 0x80u8 || b > 0xBFu8
  }

  // Whether the bytes begin with `prefix`. An empty prefix always
  // matches.
  pub fn starts_with(self: &Self, prefix: str) -> bool {
    n := str_len(prefix)
    if str_len(*self) < n {
      ret false
    }
    mut i := 0 as usize
    while i < n {
      if str_byte(*self, i) != str_byte(prefix, i) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  // Whether the bytes end with `suffix`. An empty suffix always
  // matches.
  pub fn ends_with(self: &Self, suffix: str) -> bool {
    n := str_len(suffix)
    m := str_len(*self)
    if m < n {
      ret false
    }
    mut i := 0 as usize
    while i < n {
      if str_byte(*self, m - n + i) != str_byte(suffix, i) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  // The number of non-overlapping occurrences of `needle`. An empty
  // needle counts the gaps: one more than the byte length.
  pub fn count(self: &Self, needle: str) -> usize {
    n := str_len(needle)
    m := str_len(*self)
    if n == 0 {
      ret m + 1
    }
    mut total := 0 as usize
    mut rest := *self
    loop {
      match rest.find(needle) {
        Option::Some(i) => {
          total = total + 1
          rest = str_slice(rest, i + n, str_len(rest))
        }
        Option::None => {
          ret total
        }
      }
    }
  }

  // `self` with every leading occurrence of `pat` removed. An empty
  // pattern is left alone, since it would never advance.
  pub fn trim_start_matches(self: str, pat: str) -> str {
    if str_len(pat) == 0 {
      ret self
    }
    mut s := self
    loop {
      match s.strip_prefix(pat) {
        Option::Some(rest) => {
          s = rest
        }
        Option::None => {
          ret s
        }
      }
    }
  }

  // `self` with every trailing occurrence of `pat` removed.
  pub fn trim_end_matches(self: str, pat: str) -> str {
    if str_len(pat) == 0 {
      ret self
    }
    mut s := self
    loop {
      match s.strip_suffix(pat) {
        Option::Some(rest) => {
          s = rest
        }
        Option::None => {
          ret s
        }
      }
    }
  }

  // `self` without `prefix`, or `None` when it does not start with
  // it. An empty prefix strips nothing.
  pub fn strip_prefix(self: &Self, prefix: str) -> Option<str> {
    if !self.starts_with(prefix) {
      ret Option::None
    }
    ret Option::Some(str_slice(*self, str_len(prefix), str_len(*self)))
  }

  // `self` without `suffix`, or `None` when it does not end with it.
  // An empty suffix strips nothing.
  pub fn strip_suffix(self: &Self, suffix: str) -> Option<str> {
    if !self.ends_with(suffix) {
      ret Option::None
    }
    ret Option::Some(str_slice(*self, 0, str_len(*self) - str_len(suffix)))
  }

  // Whether `needle` appears anywhere in the bytes. An empty needle
  // always matches.
  pub fn contains(self: &Self, needle: str) -> bool {
    ret self.find(needle).is_some()
  }

  // The byte offset where `needle` first appears, or `None`. An empty
  // needle is at offset zero.
  pub fn find(self: &Self, needle: str) -> Option<usize> {
    n := str_len(needle)
    m := str_len(*self)
    if n == 0 {
      ret Option::Some(0 as usize)
    }
    if n > m {
      ret Option::None
    }
    mut i := 0 as usize
    while i <= m - n {
      mut j := 0 as usize
      while j < n && str_byte(*self, i + j) == str_byte(needle, j) {
        j = j + 1
      }
      if j == n {
        ret Option::Some(i)
      }
      i = i + 1
    }
    ret Option::None
  }

  // The byte offset at or after `from` where `needle` first appears.
  // An empty needle is at `from` when that lies within the view.
  pub fn find_from(self: &Self, from: usize, needle: str) -> Option<usize> {
    n := str_len(needle)
    m := str_len(*self)
    if n == 0 {
      if from > m {
        ret Option::None
      }
      ret Option::Some(from)
    }
    if from + n > m {
      ret Option::None
    }
    mut i := from
    while i <= m - n {
      mut j := 0 as usize
      while j < n && str_byte(*self, i + j) == str_byte(needle, j) {
        j = j + 1
      }
      if j == n {
        ret Option::Some(i)
      }
      i = i + 1
    }
    ret Option::None
  }

  // The view with leading ASCII whitespace removed.
  pub fn trim_start(self: &Self) -> str {
    n := str_len(*self)
    mut i := 0 as usize
    while i < n && is_ascii_space(str_byte(*self, i)) {
      i = i + 1
    }
    ret str_slice(*self, i, n)
  }

  // The view with trailing ASCII whitespace removed.
  pub fn trim_end(self: &Self) -> str {
    n := str_len(*self)
    mut end := n
    while end > 0 && is_ascii_space(str_byte(*self, end - 1)) {
      end = end - 1
    }
    ret str_slice(*self, 0, end)
  }

  // The view with both ends trimmed. Only whitespace outside the
  // bytes is removed, so the result is a sub-view of the original.
  pub fn trim(self: &Self) -> str {
    ret self.trim_start().trim_end()
  }

  // The byte offset where `needle` last appears, or `None`. An empty
  // needle is at the end.
  pub fn rfind(self: &Self, needle: str) -> Option<usize> {
    n := str_len(needle)
    m := str_len(*self)
    if n == 0 {
      ret Option::Some(m)
    }
    if n > m {
      ret Option::None
    }
    mut i := m - n
    loop {
      mut j := 0 as usize
      while j < n && str_byte(*self, i + j) == str_byte(needle, j) {
        j = j + 1
      }
      if j == n {
        ret Option::Some(i)
      }
      if i == 0 {
        ret Option::None
      }
      i = i - 1
    }
  }

  // Whether the bytes match `other` ignoring ASCII case.
  pub fn eq_ignore_ascii_case(self: &Self, other: str) -> bool {
    n := str_len(*self)
    if n != str_len(other) {
      ret false
    }
    mut i := 0 as usize
    while i < n {
      if ascii_lower(str_byte(*self, i)) != ascii_lower(str_byte(other, i)) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  // The pieces either side of the first `delim`, or `None` when it
  // does not appear. An empty delimiter splits at the start.
  pub fn split_once(self: &Self, delim: str) -> Option<(str, str)> {
    match self.find(delim) {
      Option::Some(i) => {
        after := i + str_len(delim)
        ret Option::Some((str_slice(*self, 0, i), str_slice(*self, after, str_len(*self))))
      }
      Option::None => {
        ret Option::None
      }
    }
  }

  // A cursor over the pieces `delim` separates. An empty delimiter
  // yields the whole view once, so the cursor always advances. The
  // receiver is taken by value: a view is Copy, and a borrow of the
  // cursor's source would otherwise outlive the temporary that made
  // it.
  pub fn split(self: str, delim: str) -> SplitIter {
    ret SplitIter { rest: self, delim: delim, done: false }
  }

  // The number of times byte `b` appears.
  pub fn count_byte(self: &Self, b: u8) -> usize {
    mut n := 0 as usize
    mut i := 0 as usize
    while i < str_len(*self) {
      if str_byte(*self, i) == b {
        n = n + 1
      }
      i = i + 1
    }
    ret n
  }

  // The number of non-overlapping occurrences of `needle`. An empty
  // needle never advances, so it answers zero.
  pub fn count_matches(self: &Self, needle: str) -> usize {
    if str_len(needle) == 0 {
      ret 0
    }
    mut n := 0 as usize
    mut rest := *self
    loop {
      match rest.find(needle) {
        Option::Some(i) => {
          n = n + 1
          rest = str_slice(rest, i + str_len(needle), str_len(rest))
        }
        Option::None => {
          ret n
        }
      }
    }
  }

  // The number of scalar values the bytes hold. Every byte sequence
  // has one: an ill-formed subsequence counts as a single replacement
  // unit, the way Unicode's decoding algorithms consume the maximal
  // subpart of one.
  pub fn count_chars(self: &Self) -> usize {
    mut n := 0 as usize
    mut i := 0 as usize
    total := str_len(*self)
    while i < total {
      b := str_byte(*self, i)
      if b < 0x80u8 {
        n = n + 1
        i = i + 1
        continue
      }
      // The length a lead byte asks for, and the range its first
      // continuation byte may fall in. Zero means the byte is not a
      // lead byte at all.
      mut want := 0 as usize
      mut lo := 0x80u8
      mut hi := 0xBFu8
      if b >= 0xC2u8 && b <= 0xDFu8 {
        want = 2
      } else if b == 0xE0u8 {
        want = 3
        lo = 0xA0u8
      } else if b >= 0xE1u8 && b <= 0xECu8 {
        want = 3
      } else if b == 0xEDu8 {
        want = 3
        hi = 0x9Fu8
      } else if b >= 0xEEu8 && b <= 0xEFu8 {
        want = 3
      } else if b == 0xF0u8 {
        want = 4
        lo = 0x90u8
      } else if b >= 0xF1u8 && b <= 0xF3u8 {
        want = 4
      } else if b == 0xF4u8 {
        want = 4
        hi = 0x8Fu8
      }
      if want == 0 {
        n = n + 1
        i = i + 1
        continue
      }
      // Consume the continuation bytes that fit the scalar's ranges; a
      // sequence that stops short is one replacement unit.
      mut k := 1 as usize
      while k < want && i + k < total {
        c := str_byte(*self, i + k)
        mut fits := c >= 0x80u8 && c <= 0xBFu8
        if k == 1 {
          fits = c >= lo && c <= hi
        }
        if !fits {
          break
        }
        k = k + 1
      }
      n = n + 1
      i = i + k
    }
    ret n
  }

  // The index of the first `b`, or `None`.
  pub fn find_byte(self: &Self, b: u8) -> Option<usize> {
    mut i := 0 as usize
    while i < str_len(*self) {
      if str_byte(*self, i) == b {
        ret Option::Some(i)
      }
      i = i + 1
    }
    ret Option::None
  }

  // The index of the last `b`, or `None`.
  pub fn rfind_byte(self: &Self, b: u8) -> Option<usize> {
    mut i := str_len(*self)
    while i > 0 {
      i = i - 1
      if str_byte(*self, i) == b {
        ret Option::Some(i)
      }
    }
    ret Option::None
  }

  // Whether every byte is ASCII (below 0x80).
  pub fn is_ascii(self: &Self) -> bool {
    n := str_len(*self)
    mut i := 0 as usize
    while i < n {
      if str_byte(*self, i) >= 0x80u8 {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  // A cursor over the whitespace-separated tokens. Leading, trailing,
  // and repeated whitespace is skipped, so no token is empty.
  pub fn split_whitespace(self: str) -> SplitWhitespace {
    ret SplitWhitespace { rest: self }
  }

  // The bytes before and after `at`. The offset must land on a scalar
  // boundary within the view; anything else answers `None` rather than
  // cut a scalar in half.
  pub fn split_at(self: str, at: usize) -> Option<(str, str)> {
    m := str_len(self)
    if at > m {
      ret Option::None
    }
    if at != 0 && at != m && !self.is_char_boundary(at) {
      ret Option::None
    }
    ret Option::Some((str_slice(self, 0, at), str_slice(self, at, m)))
  }

  // A cursor over the lines, `\n`-separated, with a trailing `\r`
  // removed. A final empty line is not reported.
  pub fn lines(self: str) -> LinesIter {
    ret LinesIter { rest: self, done: false }
  }

  // A cursor over the bytes, the same order as `into_iter`. A named
  // alias so a reader does not need to know the loop desugar.
  pub fn bytes(self: str) -> StrIter {
    ret StrIter { s: self, pos: 0 }
  }

  // Byte iteration: `for b in s` reads each byte in order.
  pub fn into_iter(self: str) -> StrIter {
    ret StrIter { s: self, pos: 0 }
  }
}

// A cursor over the whitespace-separated tokens.
pub struct SplitWhitespace {
  rest: str,
}

impl SplitWhitespace {
  pub fn into_iter(self: SplitWhitespace) -> SplitWhitespace {
    ret self
  }
}

impl Iterator < str > for SplitWhitespace {
  fn next(mut self: &mut Self) -> Option<str> {
    n := str_len(self.rest)
    mut start := 0 as usize
    while start < n && is_ascii_space(str_byte(self.rest, start)) {
      start = start + 1
    }
    if start >= n {
      ret Option::None
    }
    mut end := start
    while end < n && !is_ascii_space(str_byte(self.rest, end)) {
      end = end + 1
    }
    piece := str_slice(self.rest, start, end)
    self.rest = str_slice(self.rest, end, n)
    ret Option::Some(piece)
  }
}

// A cursor over the lines a `\n` separates.
pub struct LinesIter {
  rest: str,
  done: bool,
}

impl LinesIter {
  pub fn into_iter(self: LinesIter) -> LinesIter {
    ret self
  }
}

impl Iterator < str > for LinesIter {
  fn next(mut self: &mut Self) -> Option<str> {
    if self.done || str_len(self.rest) == 0 {
      ret Option::None
    }
    match self.rest.find("\n") {
      Option::Some(i) => {
        mut line := str_slice(self.rest, 0, i)
        if line.ends_with("\r") {
          line = str_slice(line, 0, str_len(line) - 1)
        }
        self.rest = str_slice(self.rest, i + 1, str_len(self.rest))
        ret Option::Some(line)
      }
      Option::None => {
        self.done = true
        ret Option::Some(self.rest)
      }
    }
  }
}

// A consuming cursor over a string's bytes.
pub struct StrIter {
  s: str,
  pos: usize,
}

impl StrIter {
  pub fn into_iter(self: StrIter) -> StrIter {
    ret self
  }
}

impl Iterator < u8 > for StrIter {
  fn next(mut self: &mut Self) -> Option<u8> {
    if self.pos >= str_len(self.s) {
      ret Option::None
    }
    b := str_byte(self.s, self.pos)
    self.pos = self.pos + 1
    ret Option::Some(b)
  }
}

// A cursor over the pieces a delimiter separates.
pub struct SplitIter {
  rest: str,
  delim: str,
  done: bool,
}

impl SplitIter {
  pub fn into_iter(self: SplitIter) -> SplitIter {
    ret self
  }
}

impl Iterator < str > for SplitIter {
  fn next(mut self: &mut Self) -> Option<str> {
    if self.done {
      ret Option::None
    }
    if str_len(self.delim) == 0 {
      self.done = true
      ret Option::Some(self.rest)
    }
    match self.rest.find(self.delim) {
      Option::Some(i) => {
        piece := str_slice(self.rest, 0, i)
        self.rest = str_slice(self.rest, i + str_len(self.delim), str_len(self.rest))
        ret Option::Some(piece)
      }
      Option::None => {
        self.done = true
        ret Option::Some(self.rest)
      }
    }
  }
}

// A module's own tests: `#[when(testing)]` keeps them out of a build,
// and `alcy test` gathers the `#[test] fn`s into a driver that runs them.
#[when(testing)] {
  #[test] fn is_utf8_accepts_ascii_and_multibyte() {
    bytes := &[0x68u8, 0x65u8, 0x6cu8, 0x6cu8, 0x6fu8]
    assert(is_utf8(bytes), "ASCII is well-formed")
    japanese := &[0xe6u8, 0x97u8, 0xa5u8, 0xe6u8, 0x9cu8, 0xacu8, 0xe8u8, 0xa9u8, 0x9eu8]
    assert(is_utf8(japanese), "three scalars of three bytes are well-formed")
  }

  #[test] fn is_utf8_rejects_stray_continuation() {
    assert(!is_utf8(&[0x80u8]), "a continuation with no lead is none")
    assert(!is_utf8(&[0xa5u8, 0xe6u8, 0x97u8, 0xa5u8]), "a continuation can never lead")
  }

  #[test] fn is_utf8_rejects_overlong_and_surrogate() {
    assert(!is_utf8(&[0xf0u8, 0x80u8, 0x80u8]), "an overlong form is none")
    assert(!is_utf8(&[0xedu8, 0xa0u8, 0x80u8]), "a surrogate is none")
    assert(!is_utf8(&[0xf4u8, 0x90u8, 0x80u8, 0x80u8]), "one past the last scalar is none")
  }

  #[test] fn from_utf8_hands_back_the_text_when_valid() {
    bytes := &[0x68u8, 0x65u8, 0x6cu8, 0x6cu8, 0x6fu8]
    match from_utf8(bytes) {
      Result::Ok(text) => {
        assert(text == "hello", "the view rides the bytes back")
      }
      Result::Err(_) => {
        panic("an ASCII byte string is well-formed")
      }
    }
  }

  #[test] fn from_utf8_names_the_first_bad_offset() {
    bytes := &[0x68u8, 0x65u8, 0xffu8, 0x6cu8]
    match from_utf8(bytes) {
      Result::Ok(_) => {
        panic("a stray 0xff cannot decode")
      }
      Result::Err(e) => {
        assert(e.valid_up_to == 2, "the offset names the byte that broke")
      }
    }
  }

  #[test] fn empty_text_holds_no_bytes() {
    assert("".len_bytes() == 0, "there are no bytes to count")
    assert("".is_empty(), "a view of no bytes is empty")
  }

  #[test] fn one_letter_is_one_byte() {
    assert("a".len_bytes() == 1, "an ASCII letter is one byte")
    assert(!"a".is_empty(), "one byte is not nothing")
  }

  #[test] fn len_bytes_counts_bytes_and_not_scalars() {
    assert("日本".len_bytes() == 6, "two three-byte scalars are six bytes")
  }

  #[test] fn byte_reads_the_byte_at_each_end() {
    assert("abc".byte(0) == 0x61u8, "the first byte is the first letter")
    assert("abc".byte(2) == 0x63u8, "the last byte of a three-byte view sits at two")
  }

  #[test] fn byte_slice_past_the_end_is_none() {
    assert("abc".byte_slice(1, 99).is_none(), "an end past the end is refused")
    assert("abc".byte_slice(99, 100).is_none(), "a start past the end is refused")
  }

  #[test] fn byte_slice_with_the_ends_swapped_is_none() {
    assert("abc".byte_slice(2, 1).is_none(), "an end before the start names nothing")
  }

  #[test] fn is_char_boundary_accepts_the_start_and_the_end() {
    assert("日本".is_char_boundary(0), "the first byte begins a scalar")
    assert("日本".is_char_boundary(6), "the position past the last byte is a boundary")
  }

  #[test] fn is_char_boundary_rejects_a_continuation_byte() {
    assert(!"日本".is_char_boundary(1), "the second byte of a scalar continues it")
    assert(!"日本".is_char_boundary(2), "the third byte of a scalar continues it")
    assert("日本".is_char_boundary(3), "the next scalar starts at three")
  }

  #[test] fn starts_with_a_matching_prefix() {
    assert("hello".starts_with("he"), "the first two bytes are the prefix")
    assert("hello".starts_with("hello"), "a view starts with itself")
  }

  #[test] fn starts_with_of_a_missing_prefix_is_false() {
    assert(!"hello".starts_with("hi"), "a differing byte ends the match")
    assert(!"he".starts_with("hello"), "a longer prefix does not fit")
  }

  #[test] fn empty_prefix_matches_every_text() {
    assert("hello".starts_with(""), "an empty prefix asks for no byte")
  }

  #[test] fn ends_with_a_matching_suffix() {
    assert("hello".ends_with("lo"), "the last two bytes are the suffix")
    assert("hello".ends_with("hello"), "a view ends with itself")
  }

  #[test] fn ends_with_of_a_missing_suffix_is_false() {
    assert(!"hello".ends_with("no"), "a differing byte ends the match")
    assert(!"lo".ends_with("hello"), "a longer suffix does not fit")
  }

  #[test] fn empty_suffix_matches_every_text() {
    assert("hello".ends_with(""), "an empty suffix asks for no byte")
  }

  #[test] fn strip_prefix_removes_a_prefix_it_finds() {
    rest := "hello".strip_prefix("he").or_panic_with_message("the view starts with he")
    assert(rest == "llo", "what follows the prefix is the rest")
  }

  #[test] fn strip_prefix_of_a_miss_is_none() {
    assert("hello".strip_prefix("hi").is_none(), "a view without the prefix has no rest")
  }

  #[test] fn strip_prefix_of_the_whole_text_is_empty() {
    rest := "hello".strip_prefix("hello").or_panic_with_message("a view starts with itself")
    assert(rest == "", "nothing follows a prefix that is the whole view")
  }

  #[test] fn strip_prefix_of_an_empty_prefix_is_the_whole_text() {
    rest := "hello".strip_prefix("").or_panic_with_message("an empty prefix always matches")
    assert(rest == "hello", "an empty prefix strips nothing")
  }

  #[test] fn strip_suffix_removes_a_suffix_it_finds() {
    rest := "hello".strip_suffix("lo").or_panic_with_message("the view ends with lo")
    assert(rest == "hel", "what precedes the suffix is the rest")
  }

  #[test] fn strip_suffix_of_a_miss_is_none() {
    assert("hello".strip_suffix("no").is_none(), "a view without the suffix has no front")
  }

  #[test] fn strip_suffix_of_the_whole_text_is_empty() {
    rest := "hello".strip_suffix("hello").or_panic_with_message("a view ends with itself")
    assert(rest == "", "nothing precedes a suffix that is the whole view")
  }

  #[test] fn contains_finds_a_needle_in_the_middle() {
    assert("banana".contains("nan"), "the needle spans the middle bytes")
  }

  #[test] fn contains_of_an_absent_needle_is_false() {
    assert(!"banana".contains("zz"), "no byte of the needle appears")
  }

  #[test] fn empty_needle_is_contained_everywhere() {
    assert("banana".contains(""), "an empty needle asks for no byte")
    assert("".contains(""), "even a view of no bytes has the empty needle at its start")
  }

  #[test] fn find_reports_the_first_byte_offset() {
    at := "banana".find("na").or_panic_with_message("the needle is there")
    assert(at == 2, "the first na starts at two")
  }

  #[test] fn find_of_an_absent_needle_is_none() {
    assert("banana".find("zz").is_none(), "a needle that never appears has no offset")
  }

  #[test] fn find_of_the_empty_needle_is_zero() {
    at := "banana".find("").or_panic_with_message("an empty needle is at the start")
    assert(at == 0, "an empty needle appears before the first byte")
  }

  #[test] fn find_from_starts_the_search_at_the_offset() {
    at := "banana".find_from(3, "na").or_panic_with_message("the second na is there")
    assert(at == 4, "the search skips the match before from")
  }

  #[test] fn find_from_past_the_end_of_an_empty_needle_is_none() {
    assert("banana".find_from(7, "").is_none(), "a position past the view names nothing")
  }

  #[test] fn find_from_at_the_end_of_an_empty_needle_is_the_end() {
    at := "banana".find_from(6, "").or_panic_with_message("the end is within the view")
    assert(at == 6, "an empty needle at the end sits past the last byte")
  }

  #[test] fn find_byte_reports_the_first_index() {
    at := "banana".find_byte(0x6eu8).or_panic_with_message("the byte is there")
    assert(at == 2, "the first n sits at two")
  }

  #[test] fn find_byte_of_an_absent_byte_is_none() {
    assert("banana".find_byte(0x7au8).is_none(), "a byte the view lacks has no index")
  }

  #[test] fn rfind_reports_the_last_offset() {
    at := "banana".rfind("na").or_panic_with_message("the needle is there")
    assert(at == 4, "the last na starts at four")
  }

  #[test] fn rfind_of_an_absent_needle_is_none() {
    assert("banana".rfind("zz").is_none(), "a needle that never appears has no offset")
  }

  #[test] fn rfind_of_the_empty_needle_is_the_end() {
    at := "banana".rfind("").or_panic_with_message("an empty needle is at the end")
    assert(at == 6, "an empty needle sits past the last byte")
  }

  #[test] fn rfind_byte_reports_the_last_index() {
    at := "banana".rfind_byte(0x6eu8).or_panic_with_message("the byte is there")
    assert(at == 4, "the last n sits at four")
  }

  #[test] fn rfind_byte_of_an_absent_byte_is_none() {
    assert("banana".rfind_byte(0x7au8).is_none(), "a byte the view lacks has no index")
  }

  #[test] fn split_once_reports_the_bytes_either_side() {
    (key, value) := "key=value".split_once("=").or_panic_with_message("the delimiter is there")
    assert(key == "key", "the first piece precedes the delimiter")
    assert(value == "value", "the second piece follows it")
  }

  #[test] fn split_once_without_the_delimiter_is_none() {
    assert("key".split_once("=").is_none(), "a view without the delimiter does not split")
  }

  #[test] fn split_once_of_an_empty_delimiter_splits_at_the_start() {
    (empty, rest) :=
      "key"
      .split_once("")
      .or_panic_with_message("an empty delimiter is at the start")
    assert(empty == "", "nothing precedes a delimiter at the start")
    assert(rest == "key", "the whole view follows it")
  }

  #[test] fn split_yields_the_pieces_in_order() {
    mut it := "a,b,c".split(",")
    assert(
      it.next().or_panic_with_message("three pieces") == "a",
      "the first piece precedes the first delimiter",
    )
    assert(
      it.next().or_panic_with_message("two more pieces") == "b",
      "the middle piece lies between the delimiters",
    )
    assert(
      it.next().or_panic_with_message("one more piece") == "c",
      "the last piece follows the last delimiter",
    )
    assert(it.next().is_none(), "the cursor is spent after the last piece")
  }

  #[test] fn split_without_the_delimiter_yields_the_whole_view() {
    mut it := "abc".split(",")
    assert(
      it.next().or_panic_with_message("one piece") == "abc",
      "a view with no delimiter is one piece",
    )
    assert(it.next().is_none(), "the cursor is spent after it")
  }

  #[test] fn split_of_an_empty_delimiter_yields_the_whole_view_once() {
    mut it := "abc".split("")
    assert(
      it.next().or_panic_with_message("one piece") == "abc",
      "an empty delimiter separates nothing",
    )
    assert(it.next().is_none(), "the cursor advances instead of repeating")
  }

  #[test] fn split_of_an_empty_text_yields_one_empty_piece() {
    mut it := "".split(",")
    assert(
      it.next().or_panic_with_message("one piece") == "",
      "a view of no bytes is one empty piece",
    )
    assert(it.next().is_none(), "the cursor is spent after it")
  }

  #[test] fn split_whitespace_skips_runs_of_space() {
    mut it := "  a \t b  ".split_whitespace()
    assert(it.next().or_panic_with_message("two tokens") == "a", "leading runs are skipped")
    assert(
      it.next().or_panic_with_message("one more token") == "b",
      "the run between tokens is skipped",
    )
    assert(it.next().is_none(), "the trailing run yields no token")
  }

  #[test] fn split_whitespace_of_a_blank_text_yields_nothing() {
    assert("   \t ".split_whitespace().next().is_none(), "whitespace alone is no token")
  }

  #[test] fn lines_separates_on_newline() {
    mut it := "a\nb".lines()
    assert(
      it.next().or_panic_with_message("two lines") == "a",
      "the first line stops at the newline",
    )
    assert(it.next().or_panic_with_message("one more line") == "b", "the last line runs to the end")
    assert(it.next().is_none(), "the cursor is spent after the last line")
  }

  #[test] fn trailing_newline_does_not_yield_a_final_empty_line() {
    mut it := "a\n".lines()
    assert(it.next().or_panic_with_message("one line") == "a", "the newline ends the last line")
    assert(it.next().is_none(), "what follows the newline is not a line")
  }

  #[test] fn trailing_carriage_return_is_dropped() {
    mut it := "a\r\nb".lines()
    assert(
      it.next().or_panic_with_message("two lines") == "a",
      "the carriage return before the newline goes",
    )
    assert(it.next().or_panic_with_message("one more line") == "b", "the later line is untouched")
  }

  #[test] fn blank_text_yields_no_lines() {
    assert("".lines().next().is_none(), "a view of no bytes has no line")
  }

  #[test] fn bytes_yields_each_byte_in_order() {
    mut it := "abc".bytes()
    assert(
      it.next().or_panic_with_message("three bytes") == 0x61u8,
      "the first byte is the first letter",
    )
    assert(
      it.next().or_panic_with_message("two more bytes") == 0x62u8,
      "the second byte follows it",
    )
    assert(
      it.next().or_panic_with_message("one more byte") == 0x63u8,
      "the third byte is the last letter",
    )
    assert(it.next().is_none(), "the cursor is spent after the last byte")
  }

  #[test] fn bytes_of_a_multibyte_text_yields_every_byte() {
    mut n := 0 as usize
    for _ in "日本".bytes() {
      n = n + 1
    }
    assert(n == 6, "a cursor over bytes never groups a scalar")
  }

  #[test] fn trim_removes_space_at_both_ends() {
    assert("  a b  ".trim() == "a b", "only the space outside the text goes")
  }

  #[test] fn trim_start_removes_only_the_leading_space() {
    assert(" a ".trim_start() == "a ", "the trailing space stays")
  }

  #[test] fn trim_end_removes_only_the_trailing_space() {
    assert(" a ".trim_end() == " a", "the leading space stays")
  }

  #[test] fn trim_strips_every_ascii_space_byte() {
    assert("\t\n\r a \r\n\t".trim() == "a", "tab, newline, and carriage return are whitespace too")
  }

  #[test] fn trim_of_a_blank_text_is_empty() {
    assert(" \t\n\r".trim() == "", "a view of whitespace alone trims to nothing")
  }

  #[test] fn trim_of_a_text_with_no_space_is_the_text() {
    assert("abc".trim() == "abc", "nothing outside the text is there to remove")
  }

  #[test] fn trim_start_matches_removes_every_leading_hit() {
    assert("xxyxx".trim_start_matches("xx") == "yxx", "the whole leading run goes")
    assert("ababab".trim_start_matches("ab") == "", "the text can be a run all the way through")
  }

  #[test] fn trim_end_matches_removes_every_trailing_hit() {
    assert("xxyxx".trim_end_matches("xx") == "xxy", "the whole trailing run goes")
  }

  #[test] fn trim_start_matches_of_an_empty_pattern_is_the_whole_text() {
    assert("abc".trim_start_matches("") == "abc", "an empty pattern would never advance")
  }

  #[test] fn trim_end_matches_of_an_empty_pattern_is_the_whole_text() {
    assert("abc".trim_end_matches("") == "abc", "an empty pattern would never advance")
  }

  #[test] fn eq_ignore_ascii_case_matches_text_that_differs_in_case() {
    assert("Hello".eq_ignore_ascii_case("hELLO"), "each ASCII letter folds to one case")
  }

  #[test] fn eq_ignore_ascii_case_of_a_different_length_is_false() {
    assert(!"abc".eq_ignore_ascii_case("ab"), "views of different lengths differ")
  }

  #[test] fn eq_ignore_ascii_case_of_a_different_letter_is_false() {
    assert(
      !"abc".eq_ignore_ascii_case("abd"),
      "a letter that folds to neither case decides the answer",
    )
  }

  #[test] fn eq_ignore_ascii_case_folds_only_ascii() {
    assert(!"É".eq_ignore_ascii_case("é"), "a non-ASCII letter keeps its byte")
  }

  #[test] fn is_ascii_of_plain_text_is_true() {
    assert("hello ~".is_ascii(), "every byte of the text is below 0x80")
    assert("".is_ascii(), "a view of no bytes has no byte above 0x80")
  }

  #[test] fn is_ascii_of_multibyte_text_is_false() {
    assert(!"日本".is_ascii(), "a scalar above 0x7f spells a byte above 0x80")
  }

  #[test] fn split_at_reports_the_bytes_either_side() {
    match "abcdef".split_at(3) {
      Option::Some((head, tail)) => {
        assert(head == "abc", "the head runs up to the offset")
        assert(tail == "def", "the tail starts at the offset")
      }
      Option::None => {
        panic("offset 3 is a boundary")
      }
    }
  }

  #[test] fn split_at_zero_puts_the_whole_text_in_the_tail() {
    match "abcdef".split_at(0) {
      Option::Some((head, tail)) => {
        assert(head == "", "no byte precedes zero")
        assert(tail == "abcdef", "the tail is the whole view")
      }
      Option::None => {
        panic("offset 0 is a boundary")
      }
    }
  }

  #[test] fn split_at_the_end_puts_the_whole_text_in_the_head() {
    match "abcdef".split_at(6) {
      Option::Some((head, tail)) => {
        assert(head == "abcdef", "the head is the whole view")
        assert(tail == "", "no byte follows the end")
      }
      Option::None => {
        panic("offset 6 is a boundary")
      }
    }
  }

  #[test] fn split_at_inside_a_scalar_is_none() {
    assert("日本".split_at(1).is_none(), "the cut lands inside a scalar")
    assert("日本".split_at(4).is_none(), "the cut lands inside a scalar")
  }

  #[test] fn count_reports_each_occurrence() {
    assert("banana".count("a") == 3, "every a is counted")
    assert("banana".count("na") == 2, "a multi-byte needle counts the same way")
  }

  #[test] fn count_of_a_missing_needle_is_zero() {
    assert("banana".count("zz") == 0, "a needle that never appears counts nothing")
    assert("".count("a") == 0, "a view of no bytes holds no occurrence")
  }

  #[test] fn count_does_not_overlap_its_hits() {
    assert("aaa".count("aa") == 1, "the hit consumes the bytes it matched")
  }

  #[test] fn count_of_an_empty_needle_counts_the_gaps() {
    assert("abc".count("") == 4, "an empty needle sits before, between, and after the bytes")
    assert("".count("") == 1, "the one gap of an empty view is its start")
  }

  #[test] fn count_byte_reports_each_occurrence() {
    assert("banana".count_byte(0x61u8) == 3, "every a is counted")
  }

  #[test] fn count_byte_of_an_absent_byte_is_zero() {
    assert("banana".count_byte(0x7au8) == 0, "a byte the view lacks counts nothing")
  }

  #[test] fn count_matches_does_not_overlap_its_hits() {
    assert("aaa".count_matches("aa") == 1, "the hit consumes the bytes it matched")
    assert("aaaa".count_matches("aa") == 2, "the next hit starts after the first")
  }

  #[test] fn count_matches_of_an_empty_needle_is_zero() {
    assert("abc".count_matches("") == 0, "an empty needle never advances")
  }

  #[test] fn count_chars_counts_scalars_rather_than_bytes() {
    assert("日本語".count_chars() == 3, "three three-byte scalars are three characters")
    assert("hello".count_chars() == 5, "an ASCII letter is a character of its own")
    assert("".count_chars() == 0, "a view of no bytes holds no character")
  }

  #[test] fn count_chars_counts_a_truncated_sequence_once() {
    assert(
      str_slice("日", 0, 1).count_chars() == 1,
      "an ill-formed subsequence is one replacement unit",
    )
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
