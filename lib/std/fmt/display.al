// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/fmt`: how a type of one's own joins `{}`.

// `{}` reaches `display` for a type that is not one of the builtin
// convertibles. The implementation writes the prefix of its rendering
// that fits into `out` and returns the total length, so an expansion
// sizing a growable buffer learns the size from an empty slice. One
// placeholder may run the method twice (once to measure, once to
// write), so it must not have effects beyond `out`.
pub spec Display {
  fn display(self: &Self, out: &mut [u8]) -> usize;
}

// The owned string formats as its bytes, like `str` does, so a
// `format` result reaches `{}` without a detour through `as_str`.
impl Display for String {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    bytes := self.as_bytes()
    n := bytes.len()
    if n == 0 || out.len() == 0 {
      ret n
    }
    mut count := n
    if count > out.len() {
      count = out.len()
    }
    unsafe { memcopy(slice_at_mut(out, 0), slice_at(bytes, 0), count) }
    ret n
  }
}
