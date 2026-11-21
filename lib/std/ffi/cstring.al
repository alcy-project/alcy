// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// An owned byte string for a declaration that wants a terminator:
// what the C ABI calls a string with the bytes it was built from.

pub struct CString {
  bytes: Vec<u8>,
}

impl CString {
  // The bytes with a terminator, or `None` when they hold one of
  // their own. The terminator goes at the end.
  pub fn new(bytes: &[u8]) -> Option<CString> {
    mut i := 0 as usize
    while i < bytes.len() {
      if bytes[i] == 0u8 {
        ret Option::None
      }
      i = i + 1
    }
    mut buf := Vec::<u8>::with_capacity(bytes.len() + 1)
    mut j := 0 as usize
    while j < bytes.len() {
      buf.push(bytes[j])
      j = j + 1
    }
    buf.push(0u8)
    ret Option::Some(CString { bytes: buf })
  }

  // The text with a terminator, the string's own bytes.
  pub fn from_str(s: str) -> Option<CString> {
    ret CString::new(String::from(s).as_bytes())
  }

  // The bytes, without the terminator.
  pub fn as_bytes(self: &Self) -> &[u8] {
    ret self.bytes.as_slice()[0..<(self.bytes.len() - 1)]
  }

  // The bytes, terminator included.
  pub fn as_bytes_with_nul(self: &Self) -> &[u8] {
    ret self.bytes.as_slice()
  }

  // The byte count, without the terminator.
  pub fn len(self: &Self) -> usize {
    ret self.bytes.len() - 1
  }

  // Whether the content is empty.
  pub fn is_empty(self: &Self) -> bool {
    ret self.bytes.len() <= 1
  }

  // The pointer the declaration wants: the bytes, terminated.
  pub fn as_ptr(self: &Self) -> *u8 {
    ret &self.bytes.as_slice()[0] as *u8
  }
}

#[when(testing)] {
  #[test] fn new_refuses_bytes_with_a_terminator() {
    bad := [0x68u8, 0x00u8, 0x69u8]
    assert(CString::new(bad.as_slice()).is_none(), "an interior NUL is refused")
  }

  #[test] fn from_str_round_trips_through_as_bytes() {
    match CString::from_str("hello") {
      Option::Some(owned) => {
        assert(owned.len() == 5, "the bytes, terminator aside")
        assert(owned.as_bytes()[0] == 0x68u8, "the first byte is an h")
        assert(owned.as_bytes_with_nul()[5] == 0x00u8, "the terminator sits at the end")
        assert(!owned.is_empty(), "a string is not empty")
      }
      Option::None => {
        panic("a string without a NUL builds a CString")
      }
    }
  }

  #[test] fn empty_string_is_one_terminator() {
    match CString::from_str("") {
      Option::Some(owned) => {
        assert(owned.is_empty(), "no content, one terminator")
        assert(owned.as_bytes_with_nul().len() == 1, "the terminator is the only byte")
      }
      Option::None => {
        panic("an empty string builds a CString")
      }
    }
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
