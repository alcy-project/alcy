// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `fmt` package.
//
// This file is the package's facade: the re-exports below are the
// package's implicit surface. The modules beside this one hold the
// definitions and are reachable by path.
//
// The compiler recognises `write` and `format` by this package rather
// than by their names; see docs/adr/0016-suites-and-the-std-split.md. Both are expanded at compile
// time and neither declared body executes.

pub use package::display::Display;
pub use package::write::WriteOutcome;
pub use package::write::write;
pub use package::format::format;
