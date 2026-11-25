// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// alcy/std/sort: sorting over borrowed slices.
//
// The algorithms here take `&mut [T]` and a comparison, so they work on
// an owned vector, a fixed array, or any other slice without depending
// on a container. This package depends only on core.
//
// The stable sort is the default and is re-exported unqualified. The
// unstable sort is selected by path: `sort::unstable::sort`.

pub use package::stable::sort;
pub use package::stable::sort_by;
