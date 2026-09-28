// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/fmt`: formatting into a caller's buffer.

pub struct WriteOutcome { written: usize, total: usize }

pub fn write(comp fmt: str, buf: &mut [u8; 0], args: ()) -> WriteOutcome {
  panic("fmt::write must expand")
}
