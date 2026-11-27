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

#[when(testing)] {
  #[test] fn empty_iterator_collects_to_an_empty_vec() {
    src := Vec::<i32>::new()
    out := collect(src.into_iter())
    assert(out.is_empty(), "an iterator with nothing in it collects nothing")
  }

  #[test] fn collect_takes_every_element_in_order() {
    mut src := Vec::<i32>::new()
    src.push(1)
    src.push(2)
    src.push(3)
    out := collect(src.into_iter())
    assert(out.len() == 3, "one element per item the iterator handed out")
    assert(value(&out, 0) == 1, "the first item is the first element")
    assert(value(&out, 1) == 2, "the items keep their order")
    assert(value(&out, 2) == 3, "and the last item is the last element")
  }

  #[test] fn collect_into_appends_after_the_elements_already_there() {
    mut src := Vec::<i32>::new()
    src.push(2)
    src.push(3)
    mut out := Vec::<i32>::new()
    out.push(1)
    collect_into(src.into_iter(), &mut out)
    assert(out.len() == 3, "the items are added to the vec rather than replacing it")
    assert(value(&out, 0) == 1, "the element held stays first")
    assert(value(&out, 2) == 3, "the last item ends the vec")
  }

  #[test] fn partly_drained_iterator_collects_only_what_is_left() {
    mut src := Vec::<i32>::new()
    src.push(1)
    src.push(2)
    mut cursor := src.into_iter()
    _ := cursor.next()
    out := collect(cursor)
    assert(out.len() == 1, "an item already taken is not collected again")
    assert(value(&out, 0) == 2, "what is left keeps its order")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }

  fn value(v: &Vec<i32>, i: usize) -> i32 {
    ret *v.at(i).or_panic("the index is inside the vec")
  }
}
