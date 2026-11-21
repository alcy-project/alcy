// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: indexing as a capability.

// The capability behind `a[i]`: `I` is the index type and `O` the
// element type, and an index that names no element panics, the way
// array indexing behaves. The spec is sealed to the suite,
// so the standard containers are the implementations that exist.
pub spec Index<I, O> {
  fn index(self: &Self, i: I) -> &O;
}

// The capability behind `a[i] = v`: the receiver is exclusive and so
// is the result.
pub spec IndexMut<I, O> {
  fn index_mut(mut self: &mut Self, i: I) -> &mut O;
}

// A module's own tests: `#[when(testing)]` keeps them out of a build,
// and `alcy test` gathers the `#[test] fn`s into a driver that runs them.
#[when(testing)] {
  #[test] fn the_read_index_reaches_the_element_at_the_position() {
    a := [10i32, 20i32, 30i32]
    assert(a[1] == 20, "the element one past the first")
  }

  #[test] fn the_read_index_reaches_either_end() {
    a := [10i32, 20i32, 30i32]
    assert(a[0] == 10, "the first element")
    assert(a[2] == 30, "the last element")
  }

  #[test] fn the_write_index_replaces_one_element() {
    mut a := [1i32, 2i32, 3i32]
    a[1] = 9
    assert(a[1] == 9, "the written element reads back")
    assert(a[0] == 1, "the elements either side are untouched")
    assert(a[2] == 3, "and so is the element past it")
  }

  #[test] fn a_view_reads_the_element_at_the_position() {
    a := [10i32, 20i32, 30i32]
    view := a.as_slice()
    assert(view[2] == 30, "the view indexes the same elements")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
