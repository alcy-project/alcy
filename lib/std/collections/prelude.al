// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `collections` package.
//
// This file is the package's facade: the re-exports below are the
// package's implicit surface. The modules beside this one hold the
// definitions and are reachable by path.
//
// The containers that live on the heap; `alloc` holds the heap itself
// and `String`. See docs/adr/0016-suites-and-the-std-split.md.

pub use package::vec::Vec;
pub use package::vec::VecIntoIter;
pub use package::vec::VecIter;
pub use package::vec::VecIterMut;
pub use package::map::Map;
pub use package::map::MapEntryIter;
pub use package::map::MapIntoIter;
pub use package::map::MapKeys;
pub use package::map::MapValues;
pub use package::set::Set;
