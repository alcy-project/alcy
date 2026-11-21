// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: iteration as a capability.

// A source of values, consumed one at a time. The receiver is the
// cursor: `next` yields `Some` until the source is exhausted, then
// `None`. The interval types are data, so a range iterates through a
// cursor of its own rather than by itself.
pub spec Iterator<T> {
  fn next(mut self: &mut Self) -> Option<T>;
}

// An iterator that applies `f` to each value. The function's type is a
// concrete field, so `next` calls it directly; the element type is the
// adapter's own argument because a spec names its element, not the
// return of a function somewhere inside. A method's body is checked per
// instantiation, so `self.iter.next()` reaches the receiver's own
// implementation.
pub struct MapIter<I, T, U> {
  pub iter: I,
  pub f: (T) -> U,
}

impl<I: Iterator<T>, T, U> Iterator < U > for MapIter<I, T, U> {
  fn next(mut self: &mut Self) -> Option<U> {
    ret match self.iter.next() {
      Option::Some(v) => Option::Some((self.f)(v)),
      Option::None => Option::None,
    }
  }
}

// An iterator that yields only the values `f` accepts; it pulls from
// the source until one passes. The pull is a loop, so a long run of
// rejected values costs no stack.
pub struct FilterIter<I, T> {
  pub iter: I,
  pub f: (T) -> bool,
}

impl<I: Iterator<T>, T> Iterator < T > for FilterIter<I, T> {
  fn next(mut self: &mut Self) -> Option<T> {
    loop {
      match self.iter.next() {
        Option::Some(v) => {
          if (self.f)(v) {
            ret Option::Some(v)
          }
        }
        Option::None => ret Option::None,
      }
    }
  }
}

// An iterator that pairs each value with its position, counting from
// zero. The element type is the adapter's own argument: it names the
// pair the spec yields, which the input's item type alone cannot.
pub struct EnumerateIter<I, E> {
  pub iter: I,
  pub index: usize,
}

impl<I: Iterator<E>, E> Iterator < (usize, E) > for EnumerateIter<I, E> {
  fn next(mut self: &mut Self) -> Option<(usize, E)> {
    ret match self.iter.next() {
      Option::Some(v) => {
        at := self.index
        self.index = self.index + 1
        Option::Some((at, v))
      }
      Option::None => Option::None,
    }
  }
}

// An iterator that yields at most `remaining` values of its source.
pub struct TakeIter<I, T> {
  pub iter: I,
  pub remaining: usize,
}

impl<I: Iterator<T>, T> Iterator < T > for TakeIter<I, T> {
  fn next(mut self: &mut Self) -> Option<T> {
    if self.remaining == 0 {
      ret Option::None
    }
    self.remaining = self.remaining - 1
    ret self.iter.next()
  }
}

// An iterator that drops the first `remaining` values of its source on
// its first pull, then yields the rest.
pub struct SkipIter<I, T> {
  pub iter: I,
  pub remaining: usize,
  pub started: bool,
}

impl<I: Iterator<T>, T> Iterator < T > for SkipIter<I, T> {
  fn next(mut self: &mut Self) -> Option<T> {
    if !self.started {
      self.started = true
      while self.remaining > 0 {
        self.remaining = self.remaining - 1
        match self.iter.next() {
          Option::Some(_) => {}
          Option::None => ret Option::None,
        }
      }
    }
    ret self.iter.next()
  }
}

// Consumes the iterator into one value, one step at a time. The
// iterator's type and the element type are the call's to spell
// (`fold::<I, T, A>(iter, init, f)`).
pub fn fold<I: Iterator<T>, T, A>(mut iter: I, init: A, f: (A, T) -> A) -> A {
  mut acc := init
  loop {
    match iter.next() {
      Option::Some(v) => {
        acc = f(acc, v)
      }
      Option::None => break,
    }
  }
  ret acc
}

// How many values the iterator yields. The element type comes from
// the bound, so `count::<I, T>(iter)` spells both and a call with no
// turbofish infers both.
pub fn count<I: Iterator<T>, T>(mut iter: I) -> usize {
  mut n := 0 as usize
  loop {
    match iter.next() {
      Option::Some(_) => {
        n = n + 1
      }
      Option::None => break,
    }
  }
  ret n
}

// Whether any value passes. The element is consumed by the predicate,
// so the call spells both (`any::<I, T>(iter, f)`).
pub fn any<I: Iterator<T>, T>(mut iter: I, f: (T) -> bool) -> bool {
  mut found := false
  loop {
    match iter.next() {
      Option::Some(v) => {
        if f(v) {
          found = true
          break
        }
      }
      Option::None => break,
    }
  }
  ret found
}

// Whether every value passes.
pub fn all<I: Iterator<T>, T>(mut iter: I, f: (T) -> bool) -> bool {
  mut every := true
  loop {
    match iter.next() {
      Option::Some(v) => {
        if !f(v) {
          every = false
          break
        }
      }
      Option::None => break,
    }
  }
  ret every
}

// The position of the first value that passes, counting from zero.
pub fn position<I: Iterator<T>, T>(mut iter: I, f: (T) -> bool) -> Option<usize> {
  mut found: Option<usize> := Option::None
  mut at := 0 as usize
  loop {
    match iter.next() {
      Option::Some(v) => {
        if f(v) {
          found = Option::Some(at)
          break
        }
        at = at + 1
      }
      Option::None => break,
    }
  }
  ret found
}

// The first value that passes.
pub fn find<I: Iterator<T>, T>(mut iter: I, f: (T) -> bool) -> Option<T> {
  mut found: Option<T> := Option::None
  loop {
    match iter.next() {
      Option::Some(v) => {
        if f(v) {
          found = Option::Some(v)
          break
        }
      }
      Option::None => break,
    }
  }
  ret found
}

// The last value, or `None` when the iterator is empty.
pub fn last<I: Iterator<T>, T>(mut iter: I) -> Option<T> {
  mut found: Option<T> := Option::None
  loop {
    match iter.next() {
      Option::Some(v) => {
        found = Option::Some(v)
      }
      Option::None => break,
    }
  }
  ret found
}

// The adapters' constructors. The element type comes from the bound
// (`I: Iterator<T>`), so a call spells only the predicate or the count
// (`filter(v.iter(), (x: &i32) -> *x > 1)`); `map` waits for the
// return type of a closure to take part in inference.

pub fn filter<I: Iterator<T>, T>(iter: I, f: (T) -> bool) -> FilterIter<I, T> {
  ret FilterIter { iter: iter, f: f }
}

pub fn enumerate<I: Iterator<E>, E>(iter: I) -> EnumerateIter<I, E> {
  ret EnumerateIter { iter: iter, index: 0 }
}

pub fn take<I: Iterator<T>, T>(iter: I, n: usize) -> TakeIter<I, T> {
  ret TakeIter { iter: iter, remaining: n }
}

pub fn skip<I: Iterator<T>, T>(iter: I, n: usize) -> SkipIter<I, T> {
  ret SkipIter { iter: iter, remaining: n, started: false }
}

// A module's own tests: `#[when(testing)]` keeps them out of a build,
// and `alcy test` gathers the `#[test] fn`s into a driver that runs them.
#[when(testing)] {
  #[test] fn a_cursor_yields_none_after_the_last_value() {
    a := [1i32, 2i32]
    mut it := a.as_slice().iter()
    first := *it.next().or_panic_with_message("the view holds an index 0")
    second := *it.next().or_panic_with_message("the view holds an index 1")
    assert(first == 1, "the first value")
    assert(second == 2, "the second value")
    assert(it.next().is_none(), "the exhausted cursor answers none")
  }

  #[test] fn filter_keeps_only_the_values_the_predicate_accepts() {
    a := [1i32, 2i32, 3i32, 4i32]
    kept := filter(a.into_iter(), (v: i32) -> v % 2i32 == 0i32)
    assert(fold(kept, 0i32, (acc: i32, v: i32) -> acc + v) == 6, "the even values are 2 and 4")
  }

  #[test] fn filter_over_a_source_no_value_passes_yields_nothing() {
    a := [1i32, 3i32]
    assert(
      count(filter(a.into_iter(), (v: i32) -> v % 2i32 == 0i32)) == 0,
      "no value passes, so none is yielded",
    )
  }

  #[test] fn filter_over_an_empty_source_yields_nothing() {
    a := [1i32, 2i32]
    empty := a.as_slice().split_at(0).0
    assert(count(filter(empty.iter(), (v: &i32) -> *v > 0)) == 0, "there is nothing to accept")
  }

  #[test] fn enumerate_pairs_each_value_with_its_position() {
    a := [10i32, 20i32, 30i32]
    mut paired := enumerate(a.into_iter())
    first := paired.next().or_panic_with_message("the source holds three values")
    second := paired.next().or_panic_with_message("the source holds three values")
    third := paired.next().or_panic_with_message("the source holds three values")
    assert(first.0 == 0, "the position of the first value counts from zero")
    assert(first.1 == 10, "the first value itself")
    assert(second.0 == 1, "the position counts on")
    assert(third.0 == 2, "the position of the last value")
    assert(paired.next().is_none(), "the source is exhausted")
  }

  #[test] fn enumerate_of_an_empty_source_counts_nothing() {
    a := [1i32, 2i32]
    empty := a.as_slice().split_at(0).0
    assert(count(enumerate(empty.iter())) == 0, "there is no value to count")
  }

  #[test] fn take_stops_after_the_count() {
    a := [1i32, 2i32, 3i32]
    kept := take(a.into_iter(), 2)
    assert(
      fold(kept, 0i32, (acc: i32, v: i32) -> acc * 10i32 + v) == 12,
      "the first two values and nothing more",
    )
  }

  #[test] fn take_of_zero_yields_nothing() {
    a := [1i32, 2i32, 3i32]
    assert(count(take(a.into_iter(), 0)) == 0, "a count of zero ends before the first pull")
  }

  #[test] fn take_past_the_end_yields_the_whole_source() {
    a := [1i32, 2i32, 3i32]
    assert(count(take(a.into_iter(), 5)) == 3, "the source runs out first")
  }

  #[test] fn skip_drops_the_leading_values() {
    a := [1i32, 2i32, 3i32]
    kept := skip(a.into_iter(), 2)
    assert(
      fold(kept, 0i32, (acc: i32, v: i32) -> acc * 10i32 + v) == 3,
      "only the values past the count remain",
    )
  }

  #[test] fn skip_of_zero_yields_every_value() {
    a := [1i32, 2i32, 3i32]
    assert(count(skip(a.into_iter(), 0)) == 3, "the source is handed out untouched")
  }

  #[test] fn skip_past_the_end_yields_nothing() {
    a := [1i32, 2i32, 3i32]
    assert(
      count(skip(a.into_iter(), 5)) == 0,
      "the source runs out while the skipped values are dropped",
    )
  }

  #[test] fn fold_accumulates_in_order() {
    a := [1i32, 2i32, 3i32]
    total := fold(a.into_iter(), 0i32, (acc: i32, v: i32) -> acc * 10i32 + v)
    assert(total == 123, "each value lands to the right of the ones before it")
  }

  #[test] fn fold_over_an_empty_iterator_answers_the_initial_value() {
    a := [1i32, 2i32]
    empty := a.as_slice().split_at(0).0
    assert(
      fold(empty.iter(), 7i32, (acc: i32, v: &i32) -> acc + *v) == 7,
      "no value reaches the accumulator",
    )
  }

  #[test] fn count_counts_the_values() {
    a := [1i32, 2i32, 3i32]
    assert(count(a.into_iter()) == 3, "one per value")
  }

  #[test] fn count_of_an_empty_iterator_is_zero() {
    a := [1i32, 2i32]
    empty := a.as_slice().split_at(0).0
    assert(count(empty.iter()) == 0, "there is nothing to count")
  }

  #[test] fn any_accepts_one_passing_value() {
    a := [1i32, 2i32, 3i32]
    assert(any(a.into_iter(), (v: i32) -> v > 0i32), "the first value already passes")
  }

  #[test] fn any_of_no_passing_value_is_false() {
    a := [1i32, 2i32, 3i32]
    assert(!(any(a.into_iter(), (v: i32) -> v > 9i32)), "no value passes")
  }

  #[test] fn all_accepts_every_passing_value() {
    a := [1i32, 2i32, 3i32]
    assert(all(a.into_iter(), (v: i32) -> v > 0i32), "every value passes")
  }

  #[test] fn all_is_false_when_one_value_fails() {
    a := [1i32, 2i32, 3i32]
    assert(!(all(a.into_iter(), (v: i32) -> v > 1i32)), "the first value fails")
  }

  #[test] fn all_over_an_empty_iterator_is_true() {
    a := [1i32, 2i32]
    empty := a.as_slice().split_at(0).0
    assert(all(empty.iter(), (v: &i32) -> *v > 9), "no value breaks the rule")
  }

  #[test] fn position_reports_the_first_passing_value() {
    a := [1i32, 2i32, 3i32]
    assert(
      position(a.into_iter(), (v: i32) -> v > 1i32).or_panic_with_message("a value passes") == 1,
      "the position counts from zero",
    )
  }

  #[test] fn position_of_no_passing_value_is_none() {
    a := [1i32, 2i32, 3i32]
    assert(position(a.into_iter(), (v: i32) -> v > 9i32).is_none(), "no value passes")
  }

  #[test] fn find_answers_the_first_passing_value() {
    a := [1i32, 2i32, 3i32]
    assert(
      find(a.into_iter(), (v: i32) -> v > 1i32).or_panic_with_message("a value passes") == 2,
      "the value, not its position",
    )
  }

  #[test] fn find_without_a_match_is_none() {
    a := [1i32, 2i32, 3i32]
    assert(find(a.into_iter(), (v: i32) -> v > 9i32).is_none(), "no value passes")
  }

  #[test] fn last_answers_the_final_value() {
    a := [1i32, 2i32, 3i32]
    assert(
      last(a.into_iter()).or_panic_with_message("the source yields values") == 3,
      "the value the source ends on",
    )
  }

  #[test] fn last_of_an_empty_iterator_is_none() {
    a := [1i32, 2i32]
    empty := a.as_slice().split_at(0).0
    assert(last(empty.iter()).is_none(), "there is no last value")
  }

  #[test] fn map_applies_the_function_to_each_value() {
    a := [1i32, 2i32, 3i32]
    it: MapIter<ArrayIntoIter<i32, 3>, i32, i32> :=
      MapIter { iter: a.into_iter(), f: (v: i32) -> v * 2i32 }
    assert(
      fold(it, 0i32, (acc: i32, v: i32) -> acc + v) == 12,
      "each value was doubled before it left",
    )
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
