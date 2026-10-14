// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/io`: writing to the standard output.
//
// `print` and `println` are compiler-expanded like `fmt::format`: the
// format string is comp-known, the call's remaining arguments are
// collected at compile time, and the declared body never runs
// (ADR-0055). `println` writes one newline after the expansion.

pub fn print(comp fmt: str, args: ..) {
  panic("io::print must expand")
}

pub fn println(comp fmt: str, args: ..) {
  panic("io::println must expand")
}

pub fn eprint(comp fmt: str, args: ..) {
  panic("io::eprint must expand")
}

pub fn eprintln(comp fmt: str, args: ..) {
  panic("io::eprintln must expand")
}
