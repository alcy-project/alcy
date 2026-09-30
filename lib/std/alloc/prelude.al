// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `alloc` package.
//
// This file is the package's facade: the re-exports below are the
// package's implicit surface. The modules beside this one hold the
// definitions and are reachable by path.
//
// The heap and the values that live on it. `Vec` and `String` are here
// rather than in core because they are heap-backed; see docs/adr/0016-suites-and-the-std-split.md.

pub use super::heap::alloc;
pub use super::heap::dealloc;
pub use super::heap::elem_ptr;
pub use super::heap::elem_ref;
pub use super::heap::uninit_write;
pub use super::heap::uninit_assume;
pub use super::heap::uninit_ref;
pub use super::string::String;
pub use super::vec::Vec;
pub use super::map::Map;
pub use super::set::Set;
