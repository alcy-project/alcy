// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/sys`: what a failed call reported.

// The kind of a failed call: the portable half of the error. The
// runtime answers a stable kind code for the number it reported, so
// the table is the target's own (ADR-0128).
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
// own (ADR-0128).
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
