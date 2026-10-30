// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/sys`: the descriptor a call names.

// A descriptor the system knows. The nominal type keeps a descriptor
// from mixing with the counts and flags around it.
pub struct Fd {
  number: i32,
}

impl Fd {
  // The three descriptors the process starts with.
  pub fn stdin() -> Fd {
    ret Fd { number: 0 }
  }

  pub fn stdout() -> Fd {
    ret Fd { number: 1 }
  }

  pub fn stderr() -> Fd {
    ret Fd { number: 2 }
  }

  // The number the calls take, for a program that obtained a
  // descriptor outside this package.
  pub fn raw(self: &Self) -> i32 {
    ret self.number
  }

  // A descriptor from a number the system named.
  pub fn from_raw(number: i32) -> Fd {
    ret Fd { number: number }
  }
}
