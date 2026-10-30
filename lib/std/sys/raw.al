// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/sys`: the faithful descriptor calls.

use package::error::error_from;

// One `write(2)`, answering the byte count or the negated error
// number the call reported. The compiler provides it per target
// (ADR-0128).
intrinsic fn sys_write(fd: i32, buf: str) -> i64;

// One `read(2)`, the same way. The caller guarantees room for
// `out_len` bytes at `out`, which is why the declaration carries
// `unsafe` (ADR-0050).
unsafe intrinsic fn sys_read(fd: i32, out: &mut u8, out_len: usize) -> i64;

// One `write(2)`: the bytes it accepted, or the error it reported. A
// short write is the caller's to see; nothing is retried here, and
// the print path retries in its own loop.
pub fn write(fd: Fd, bytes: str) -> Result<usize, OsError> {
  n := sys_write(fd.raw(), bytes)
  if n < 0 {
    ret Result::Err(error_from((0 - n) as i32))
  }
  ret Result::Ok(n as usize)
}

// One `read(2)`: the bytes it filled, zero at the end of input, or
// the error it reported. A short read is the caller's to see.
pub fn read(fd: Fd, out: &mut [u8]) -> Result<usize, OsError> {
  n_out := out.len()
  if n_out == 0 {
    ret Result::Ok(0)
  }
  n := unsafe { sys_read(fd.raw(), slice_at_mut(out, 0), n_out) }
  if n < 0 {
    ret Result::Err(error_from((0 - n) as i32))
  }
  ret Result::Ok(n as usize)
}
