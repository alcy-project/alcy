// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/io`: reading and writing whole files. The path is the
// platform's own spelling, and the path is not interpreted.

use path::prelude::Path;

// The byte count of the file at `path`, or -1 when it cannot be
// opened. The path is NUL-terminated in a temporary slot.
pub unsafe intrinsic fn file_size(path: &[u8]) -> i64;

// Reads up to `out_len` bytes from the file at `path` into the bytes
// `out` names, answering the bytes read or -1 when it cannot be
// opened. The caller must have room for `out_len` bytes at `out`.
pub unsafe intrinsic fn read_file_into(path: &[u8], out: &mut u8, out_len: usize) -> i64;

// Writes `data` to the file at `path`, answering the bytes written or
// -1 when it cannot be opened. The path is NUL-terminated in a
// temporary slot and an existing file is truncated.
pub unsafe intrinsic fn write_file_all(path: &[u8], data: str) -> i64;

// The whole file at `path` as bytes, or `None` when it cannot be
// read.
pub fn read_bytes(path: &Path) -> Option<Vec<u8>> {
  bytes := path.encoded_bytes()
  n := unsafe { file_size(bytes.as_slice()) }
  if n < 0 {
    ret Option::None
  }
  mut buf := Vec::<u8>::with_capacity(n as usize)
  {
    spare := unsafe { buf.spare_capacity_mut() }
    len := spare.len()
    m := unsafe { read_file_into(bytes.as_slice(), slice_at_mut(spare, 0), len) }
    if m < 0 {
      ret Option::None
    }
    unsafe { buf.set_len(m as usize) }
  }
  ret Option::Some(buf)
}

// The whole file at `path` as a string, or `None` when it cannot be
// read or its bytes are not well-formed UTF-8.
pub fn read_file(path: &Path) -> Option<String> {
  match read_bytes(path) {
    Option::Some(bytes) => {
      ret String::from_slice(bytes.as_slice())
    }
    Option::None => {
      ret Option::None
    }
  }
}

// Writes `contents` to the file at `path`, answering whether it could.
// An existing file is truncated.
pub fn write_file(path: &Path, contents: str) -> bool {
  bytes := path.encoded_bytes()
  ret unsafe { write_file_all(bytes.as_slice(), contents) } >= 0
}
