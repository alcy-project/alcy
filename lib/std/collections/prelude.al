// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `collections` package.
//
// This file is the package's facade: the re-exports below are the
// package's implicit surface. The modules beside this one hold the
// definitions and are reachable by path.
//
// The containers with lookup algorithms, `Map` and `Set`. The heap is
// `alloc`'s, the string `text`'s, and the buffer `Vec` is `alloc`'s
// too; see docs/adr/0016-suites-and-the-std-split.md.

pub use package::map::Map;
pub use package::map::MapEntryIter;
pub use package::map::MapIntoIter;
pub use package::map::MapKeys;
pub use package::map::MapValues;
pub use package::map::Entry;
pub use package::set::Set;
