// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: the `str` view over bytes.

pub intrinsic fn str_len(s: str) -> usize;

pub intrinsic fn str_byte(s: str, i: usize) -> u8;

pub intrinsic fn str_slice(s: str, start: usize, end: usize) -> str;

pub unsafe intrinsic fn str_from_parts(ptr: &u8, len: usize) -> str;

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
  if b == 0x20u8 { ret true }
  if b == 0x09u8 { ret true }
  if b == 0x0au8 { ret true }
  if b == 0x0bu8 { ret true }
  if b == 0x0cu8 { ret true }
  if b == 0x0du8 { ret true }
  ret false
}

impl str {
  // The byte length.
  pub fn len(self: &Self) -> usize {
    ret str_len(*self)
  }

  pub fn is_empty(self: &Self) -> bool {
    ret str_len(*self) == 0
  }

  // The byte at `at`; in bounds exactly when `at < self.len()`.
  pub fn byte(self: &Self, at: usize) -> u8 {
    ret str_byte(*self, at)
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
        Option::None => { ret total }
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
        Option::Some(rest) => { s = rest }
        Option::None => { ret s }
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
        Option::Some(rest) => { s = rest }
        Option::None => { ret s }
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
        ret Option::Some(
            (str_slice(*self, 0, i), str_slice(*self, after, str_len(*self))))
      }
      Option::None => { ret Option::None }
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

  // Parses a decimal integer with an optional leading `+` or `-`.
  // `None` when the bytes are not a number or the magnitude overflows
  // `i64`, so no wrap is read as a value.
  pub fn parse_i64(self: &Self) -> Option<i64> {
    n := str_len(*self)
    if n == 0 {
      ret Option::None
    }
    mut i := 0 as usize
    mut negative := false
    b := str_byte(*self, 0)
    if b == 0x2du8 {
      negative = true
      i = 1
    } else if b == 0x2bu8 {
      i = 1
    }
    if i >= n {
      ret Option::None
    }
    mut limit := 9223372036854775807u64
    if negative {
      limit = 9223372036854775808u64
    }
    mut value := 0u64
    while i < n {
      d := str_byte(*self, i)
      if d < 0x30u8 || d > 0x39u8 {
        ret Option::None
      }
      digit := (d - 0x30u8) as u64
      if value > (limit - digit) / 10u64 {
        ret Option::None
      }
      value = value * 10u64 + digit
      i = i + 1
    }
    if negative {
      if value == 9223372036854775808u64 {
        ret Option::Some(0i64 - 9223372036854775807i64 - 1i64)
      }
      ret Option::Some(0i64 - (value as i64))
    }
    ret Option::Some(value as i64)
  }

  // Parses an unsigned decimal integer, with an optional leading `+`.
  // `None` when the bytes are not a number or the magnitude overflows
  // `u64`.
  pub fn parse_u64(self: &Self) -> Option<u64> {
    n := str_len(*self)
    if n == 0 {
      ret Option::None
    }
    mut i := 0 as usize
    if str_byte(*self, 0) == 0x2bu8 {
      i = 1
    }
    if i >= n {
      ret Option::None
    }
    mut value := 0u64
    while i < n {
      d := str_byte(*self, i)
      if d < 0x30u8 || d > 0x39u8 {
        ret Option::None
      }
      digit := (d - 0x30u8) as u64
      if value > (18446744073709551615u64 - digit) / 10u64 {
        ret Option::None
      }
      value = value * 10u64 + digit
      i = i + 1
    }
    ret Option::Some(value)
  }

  // Parses a decimal integer that must also fit `i32`.
  pub fn parse_i32(self: &Self) -> Option<i32> {
    match self.parse_i64() {
      Option::Some(v) => {
        if v < 0i64 - 2147483647i64 - 1i64 || v > 2147483647i64 {
          ret Option::None
        }
        ret Option::Some(v as i32)
      }
      Option::None => {
        ret Option::None
      }
    }
  }

  // Parses a decimal integer that must also fit `u32`.
  pub fn parse_u32(self: &Self) -> Option<u32> {
    match self.parse_u64() {
      Option::Some(v) => {
        if v > 4294967295u64 {
          ret Option::None
        }
        ret Option::Some(v as u32)
      }
      Option::None => {
        ret Option::None
      }
    }
  }

  // Parses a decimal floating point number with an optional sign and
  // fractional part. An exponent is not read. `None` when the bytes
  // are not a number or the digits pass what a `u64` numerator holds.
  pub fn parse_f64(self: &Self) -> Option<f64> {
    n := str_len(*self)
    if n == 0 {
      ret Option::None
    }
    mut i := 0 as usize
    mut negative := false
    b := str_byte(*self, 0)
    if b == 0x2du8 {
      negative = true
      i = 1
    } else if b == 0x2bu8 {
      i = 1
    }
    if i >= n {
      ret Option::None
    }
    mut numerator := 0u64
    mut scale := 1u64
    mut seen := false
    mut point := false
    while i < n {
      d := str_byte(*self, i)
      if d == 0x2eu8 {
        if point {
          ret Option::None
        }
        point = true
        i = i + 1
        continue
      }
      if d < 0x30u8 || d > 0x39u8 {
        ret Option::None
      }
      digit := (d - 0x30u8) as u64
      if numerator > (18446744073709551615u64 - digit) / 10u64 {
        ret Option::None
      }
      numerator = numerator * 10u64 + digit
      if point {
        if scale > 1844674407370955161u64 {
          ret Option::None
        }
        scale = scale * 10u64
      }
      seen = true
      i = i + 1
    }
    if !seen {
      ret Option::None
    }
    mut value := (numerator as f64) / (scale as f64)
    if negative {
      value = 0.0f64 - value
    }
    ret Option::Some(value)
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

  // The bytes before and after `at`. The caller knows `at` is a
  // boundary it chose; a byte offset is accepted as given.
  pub fn split_at(self: str, at: usize) -> (str, str) {
    ret (str_slice(self, 0, at), str_slice(self, at, str_len(self)))
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

impl Iterator<str> for SplitWhitespace {
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

impl Iterator<str> for LinesIter {
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

impl Iterator<u8> for StrIter {
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

impl Iterator<str> for SplitIter {
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
        self.rest =
            str_slice(self.rest, i + str_len(self.delim), str_len(self.rest))
        ret Option::Some(piece)
      }
      Option::None => {
        self.done = true
        ret Option::Some(self.rest)
      }
    }
  }
}
