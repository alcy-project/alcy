// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `io` package.
//
// This file is the package's facade: the re-exports below are the
// package's implicit surface. The modules beside this one hold the
// definitions and are reachable by path.
//
// The compiler recognises `print`, `println`, `eprint`, and `eprintln`
// by this package rather than by their names; all are expanded at
// compile time and none of the declared bodies executes. The `e` forms
// write stderr. See docs/adr/0016-suites-and-the-std-split.md.

pub use package::print::print;
pub use package::print::println;
pub use package::print::eprint;
pub use package::print::eprintln;
