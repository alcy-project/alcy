// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `text` package.
//
// This file is the package's facade: the re-exports below are the
// package's implicit surface. The modules beside this one hold the
// definitions and are reachable by path.
//
// Owned text. The byte view is `core`'s `str`; the buffer is `alloc`'s;
// the Unicode algorithms above this package are `unicode`'s.

pub use package::string::String;
pub use package::string::join;
