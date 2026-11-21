// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// A view over bytes that end in a terminator, the shape a
// declaration that borrows a string hands back.

use core::string::from_utf8;
use core::string::Utf8Error;

pub struct CStr {
  ptr: *u8,
}

impl CStr {
  // The pointer, borrowed: the caller guarantees it names a
  // NUL-terminated sequence that outlives the view, and that the
  // memory stays initialized for that long.
  pub unsafe fn from_ptr(ptr: *u8) -> CStr {
    ret CStr { ptr: ptr }
  }

  // The bytes before the terminator, as a view.
  pub fn as_bytes(self: &Self) -> &[u8] {
    mut n := 0 as usize
    loop {
      b := unsafe { *ptr_offset(self.ptr, n as isize) }
      if b == 0u8 {
        ret unsafe { slice_from_parts(&*(self.ptr as *u8), n) }
      }
      n = n + 1
    }
  }

  // The byte count before the terminator.
  pub fn len(self: &Self) -> usize {
    ret self.as_bytes().len()
  }

  // Whether the content is empty.
  pub fn is_empty(self: &Self) -> bool {
    ret self.len() == 0
  }

  // The bytes before the terminator, checked as UTF-8.
  pub fn as_str(self: &Self) -> Result<str, Utf8Error> {
    ret from_utf8(self.as_bytes())
  }
}

#[when(testing)] {
  #[test] fn from_ptr_reads_back_what_the_owner_put_in() {
    owned := CString::from_str("borrowed").or_panic_with_message("a plain string")
    view := CStr::from_ptr(owned.as_ptr())
    match view.as_str() {
      Result::Ok(text) => {
        assert(text.len_bytes() == 8, "the view is as long as the content")
        assert(text == "borrowed", "the view holds the bytes")
      }
      Result::Err(_) => {
        panic("the bytes are UTF-8")
      }
    }
  }

  #[test] fn the_length_stops_at_the_terminator() {
    owned :=
      CString::from_str("a string long enough to notice")
      .or_panic_with_message("a plain string")
    view := CStr::from_ptr(owned.as_ptr())
    assert(view.len() == owned.len(), "the view counts as far as the terminator")
  }

  #[test] fn an_empty_view_is_one_terminator() {
    owned := CString::from_str("").or_panic_with_message("an empty string")
    view := CStr::from_ptr(owned.as_ptr())
    assert(view.is_empty(), "the lone terminator reads as empty")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
