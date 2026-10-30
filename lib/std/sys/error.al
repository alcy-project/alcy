// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/sys`: what a failed call reported.

// The kind of a failed call: the portable half of the error. The
// numbers behind these are the first target's (Linux); another target
// extends the table with its own numbers while the kind stays the
// program's vocabulary (ADR-0117).
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
// the kind it maps to. A number outside the documented set stays in
// `code` and answers `Other`.
pub struct OsError {
  pub code: i32,
  pub kind: ErrorKind,
}

// The kind of a raw error number.
pub fn kind_of(code: i32) -> ErrorKind {
  if code == 4 {
    ret ErrorKind::Interrupted
  }
  if code == 11 {
    ret ErrorKind::WouldBlock
  }
  if code == 2 {
    ret ErrorKind::NotFound
  }
  if code == 1 || code == 13 {
    ret ErrorKind::PermissionDenied
  }
  if code == 32 {
    ret ErrorKind::BrokenPipe
  }
  if code == 22 {
    ret ErrorKind::InvalidInput
  }
  ret ErrorKind::Other
}

// The error a call answered as `-(errno)`.
pub fn error_from(code: i32) -> OsError {
  ret OsError { code: code, kind: kind_of(code) }
}
