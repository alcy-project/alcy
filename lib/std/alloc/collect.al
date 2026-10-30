// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/alloc`: draining an iterator into a `Vec`.

// Pushes every value into a container the caller owns. The element
// type comes from the bound (`collect_into::<I, T>(iter, out)`).
pub fn collect_into<I: Iterator<T>, T>(mut iter: I, out: &mut Vec<T>) {
  loop {
    match iter.next() {
      Option::Some(v) => {
        out.push(v)
      }
      Option::None => break,
    }
  }
}

// `collect`'s shape: the item type comes from the bound, so the call
// spells nothing (`collect(v.iter())`). `T` appears only in
// `I: Iterator<T>`, and the checker infers it from the iterator's
// implementation.
pub fn collect<I: Iterator<T>, T>(mut iter: I) -> Vec<T> {
  mut out := Vec::<T>::new()
  collect_into::<I, T>(iter, &mut out)
  ret out
}
