// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `collections` package.
//
// This file is the package's facade: the re-exports below are the
// package's implicit surface. The modules beside this one hold the
// definitions and are reachable by path.
//
// The containers with lookup algorithms, `HashMap` and `HashSet`. The
// heap is `alloc`'s, the string `text`'s, and the buffer `Vec` is
// `alloc`'s too.

pub use package::hash_map::HashMap;
pub use package::hash_map::HashMapEntryIter;
pub use package::hash_map::HashMapIntoIter;
pub use package::hash_map::HashMapKeys;
pub use package::hash_map::HashMapValues;
pub use package::hash_map::Entry;
pub use package::hash_map::into_iter_of;
pub use package::hash_set::HashSet;
pub use package::hash_set::HashSetIntoIter;
pub use package::hash_set::keys_of;
