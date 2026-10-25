// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `alloc` package.
//
// This file is the package's facade: the re-exports below are the
// package's implicit surface. The modules beside this one hold the
// definitions and are reachable by path.
//
// The heap and the raw buffers, `Box` and `Vec`. The string lives in
// the `text` package above this one and the lookup structures in
// `collections`; see docs/adr/0016-suites-and-the-std-split.md.

pub use package::box::Box;
pub use package::heap::alloc;
pub use package::heap::dealloc;
pub use package::heap::doubled_capacity;
pub use package::heap::elem_ptr;
pub use package::heap::elem_ref;
pub use package::heap::uninit_write;
pub use package::heap::uninit_assume;
pub use package::heap::uninit_ref;
pub use package::vec::Vec;
pub use package::vec::VecIntoIter;
pub use package::vec::VecIter;
pub use package::vec::VecIterMut;
