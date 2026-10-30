// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `sys` package.
//
// This file is the package's facade: the re-exports below are the
// package's implicit surface. The modules beside this one hold the
// definitions and are reachable by path.
//
// The calls stay off the facade: `read` and `write` already name a
// core intrinsic and `fmt`'s expanded write, and a suite's implicit
// surfaces do not repeat a name (ADR-0016). They are reached by path
// (`sys::raw::read`, `sys::raw::write`).

pub use package::args::arg;
pub use package::args::arg_count;
pub use package::error::ErrorKind;
pub use package::error::OsError;
pub use package::fd::Fd;
