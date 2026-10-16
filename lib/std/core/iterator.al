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

impl<I, T, U> Iterator < U > for MapIter<I, T, U> {
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

impl<I, T> Iterator < T > for FilterIter<I, T> {
  fn next(mut self: &mut Self) -> Option<T> {
    ret match self.iter.next() {
      Option::Some(v) => if (self.f)(v) {
        Option::Some(v)
      } else {
        self.next()
      }
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

impl<I, E> Iterator < (usize, E) > for EnumerateIter<I, E> {
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

impl<I, T> Iterator < T > for TakeIter<I, T> {
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

impl<I, T> Iterator < T > for SkipIter<I, T> {
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

// How many values the iterator yields. The element type does not
// appear, so `count::<I>(iter)` is enough.
pub fn count<I>(mut iter: I) -> usize {
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
pub fn any<I, T>(mut iter: I, f: (T) -> bool) -> bool {
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
pub fn all<I, T>(mut iter: I, f: (T) -> bool) -> bool {
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
pub fn position<I, T>(mut iter: I, f: (T) -> bool) -> Option<usize> {
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
pub fn find<I, T>(mut iter: I, f: (T) -> bool) -> Option<T> {
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
pub fn last<I, T>(mut iter: I) -> Option<T> {
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

// Pushes every value into a container the caller owns. The target's
// element type is what the call spells (`collect_into::<I, T>(iter,
// out)`), which is the part `collect` would ask a bound for.
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

// `collect`'s shape: the item type comes from the bound, so the call
// spells nothing (`collect(v.iter())`). `T` appears only in
// `I: Iterator<T>`, and the checker infers it from the iterator's
// implementation.
pub fn collect<I: Iterator<T>, T>(mut iter: I) -> Vec<T> {
  mut out := Vec::<T>::new()
  collect_into::<I, T>(iter, &mut out)
  ret out
}

pub fn collect_into<I, T>(mut iter: I, out: &mut Vec<T>) {
  loop {
    match iter.next() {
      Option::Some(v) => {
        out.push(v)
      }
      Option::None => break,
    }
  }
}
