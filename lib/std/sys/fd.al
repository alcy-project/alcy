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

test {
  test stdin_names_descriptor_zero() {
    assert(Fd::stdin().raw() == 0, "the standard input is descriptor zero")
  }

  test stdout_names_descriptor_one() {
    assert(Fd::stdout().raw() == 1, "the standard output is descriptor one")
  }

  test stderr_names_descriptor_two() {
    assert(Fd::stderr().raw() == 2, "the standard error is descriptor two")
  }

  test from_raw_keeps_the_number_it_was_given() {
    assert(Fd::from_raw(7).raw() == 7, "a descriptor names the number it was built from")
  }

  test from_raw_round_trips_the_number_of_a_known_descriptor() {
    assert(Fd::from_raw(Fd::stdout().raw()).raw() == 1, "the number comes back out unchanged")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
