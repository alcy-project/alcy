// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/io`: reading and writing whole files. The caller names the
// file the way the platform does; the path is not interpreted.

// The byte count of the file at `path`, or -1 when it cannot be
// opened. The path is NUL-terminated in a temporary slot.
pub unsafe intrinsic fn file_size(path: str) -> i64;

// Reads up to `out_len` bytes from the file at `path` into the bytes
// `out` names, answering the bytes read or -1 when it cannot be
// opened. The caller must have room for `out_len` bytes at `out`.
pub unsafe intrinsic fn read_file_into(path: str, out: &mut u8, out_len: usize) -> i64;

// Writes `data` to the file at `path`, answering the bytes written or
// -1 when it cannot be opened. The path is NUL-terminated in a
// temporary slot and an existing file is truncated.
pub unsafe intrinsic fn write_file_all(path: str, data: str) -> i64;

// The whole file at `path`, or `None` when it cannot be read.
pub fn read_file(path: str) -> Option<String> {
  n := unsafe { file_size(path) }
  if n < 0 {
    ret Option::None
  }
  mut s := String::with_capacity(n as usize)
  {
    spare := s.spare_capacity_mut()
    len := spare.len()
    m := unsafe { read_file_into(path, slice_at_mut(spare, 0), len) }
    if m < 0 {
      ret Option::None
    }
    unsafe { s.set_len(m as usize) }
  }
  ret Option::Some(s)
}

// Writes `contents` to the file at `path`, answering whether it could.
// An existing file is truncated.
pub fn write_file(path: str, contents: str) -> bool {
  ret unsafe { write_file_all(path, contents) } >= 0
}
