// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/io`: reading and writing whole files. The caller names the
// file the way the platform does; the path is not interpreted.

// The whole file at `path`, or `None` when it cannot be read.
pub fn read_file(path: str) -> Option<String> {
  ret String::read_file(path)
}

// Writes `contents` to the file at `path`, answering whether it could.
// An existing file is truncated.
pub fn write_file(path: str, contents: str) -> bool {
  ret String::write_file(path, contents)
}
