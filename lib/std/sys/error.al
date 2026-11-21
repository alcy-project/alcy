// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/sys`: what a failed call reported.

// The kind of a failed call: the portable half of the error. The
// runtime answers a stable kind code for the number it reported, so
// the table is the target's own.
pub enum ErrorKind {
  Interrupted,
  WouldBlock,
  NotFound,
  PermissionDenied,
  BrokenPipe,
  InvalidInput,
  Other,
}

// The error a call reported: the raw number the runtime answered and
// the kind it maps to. The number is target-specific; the kind is
// the program's vocabulary.
pub struct OsError {
  pub code: i32,
  pub kind: ErrorKind,
}

// The runtime maps the target's errno number to a stable kind code:
//   0 = Other, 1 = Interrupted, 2 = WouldBlock, 3 = NotFound,
//   4 = PermissionDenied, 5 = BrokenPipe, 6 = InvalidInput.
// The compiler provides it per target, so the table is the target's
// own.
intrinsic fn error_kind(code: i32) -> i32;

// The kind of a raw error number.
pub fn kind_of(code: i32) -> ErrorKind {
  k := error_kind(code)
  if k == 1 {
    ret ErrorKind::Interrupted
  }
  if k == 2 {
    ret ErrorKind::WouldBlock
  }
  if k == 3 {
    ret ErrorKind::NotFound
  }
  if k == 4 {
    ret ErrorKind::PermissionDenied
  }
  if k == 5 {
    ret ErrorKind::BrokenPipe
  }
  if k == 6 {
    ret ErrorKind::InvalidInput
  }
  ret ErrorKind::Other
}

// The error a call answered as `-(errno)`.
pub fn error_from(code: i32) -> OsError {
  ret OsError { code: code, kind: kind_of(code) }
}

#[when(testing)] {
  #[test] fn error_from_keeps_the_number_the_call_reported() {
    assert(error_from(13).code == 13, "the raw number is kept as it arrived")
  }

  #[test] fn error_from_keeps_a_number_no_kind_covers() {
    assert(error_from(9999999).code == 9999999, "no number is clamped on the way in")
  }

  #[test] fn kind_of_a_number_outside_the_table_is_other() {
    assert(kind_is_other(kind_of(9999999)), "a number the table does not name is Other")
  }

  #[test] fn error_from_takes_the_kind_from_the_number() {
    assert(kind_is_other(error_from(9999999).kind), "an error carries the kind of its number")
  }

  fn kind_is_other(kind: ErrorKind) -> bool {
    ret match kind {
      ErrorKind::Other => true,
      _ => false,
    }
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
