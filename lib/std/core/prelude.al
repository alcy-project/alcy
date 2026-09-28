// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `core` package.
//
// This file is the package's facade: the re-exports below are the
// package's implicit surface. The modules beside this one hold the
// definitions and are reachable by path.
//
// Core is the one package of the suite with no dependencies; see
// docs/adr/0016.

pub use super::mem::memcopy;
pub use super::mem::panic;
pub use super::mem::print;
pub use super::mem::println;
pub use super::mem::size_of;
pub use super::mem::align_of;
pub use super::text::str_len;
pub use super::text::str_byte;
pub use super::text::str_slice;
pub use super::text::str_from_parts;
pub use super::option::Option;
pub use super::option::Result;
