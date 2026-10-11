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

impl<I, T, U> Iterator<U> for MapIter<I, T, U> {
  fn next(mut self: &mut Self) -> Option<U> {
    ret match self.iter.next() {
      Option::Some(v) => Option::Some((self.f)(v)),
      Option::None => Option::None,
    }
  }
}

// An iterator that yields only the values `f` accepts; it pulls from
// the source until one passes.
pub struct FilterIter<I, T> {
  pub iter: I,
  pub f: (T) -> bool,
}

impl<I, T> Iterator<T> for FilterIter<I, T> {
  fn next(mut self: &mut Self) -> Option<T> {
    ret match self.iter.next() {
      Option::Some(v) => if (self.f)(v) {
        Option::Some(v)
      } else {
        self.next()
      },
      Option::None => Option::None,
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

impl<I, E> Iterator<E> for EnumerateIter<I, E> {
  fn next(mut self: &mut Self) -> Option<E> {
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

impl<I, T> Iterator<T> for TakeIter<I, T> {
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

impl<I, T> Iterator<T> for SkipIter<I, T> {
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
pub fn fold<I, T, A>(mut iter: I, init: A, f: (A, T) -> A) -> A {
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
