// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: the option type.

pub enum Option<T> {
  Some(T),
  None,
}

impl<T> Option<T> {
  // Consulted through a shared borrow, so the option is still there
  // to ask again.
  pub fn is_some(self: &Self) -> bool {
    ret match *self {
      Option::Some(_) => true,
      Option::None => false,
    }
  }

  pub fn is_none(self: &Self) -> bool {
    ret match *self {
      Option::Some(_) => false,
      Option::None => true,
    }
  }

  // Panics on `None`. Prefer `is_some` or `?` where absence is
  // expected rather than exceptional.
  pub fn unwrap(self: Self) -> T {
    ret match self {
      Option::Some(v) => v,
      Option::None => panic("called `Option::unwrap` on a `None` value"),
    }
  }

  pub fn expect(self: Self, msg: str) -> T {
    ret match self {
      Option::Some(v) => v,
      Option::None => panic(msg),
    }
  }

  // Yields the contained value or `default` when absent.
  pub fn or(self: Self, default: T) -> T {
    ret match self {
      Option::Some(v) => v,
      Option::None => default,
    }
  }

  // The contained value, or `default` when absent.
  pub fn unwrap_or(self: Self, default: T) -> T {
    ret self.or(default)
  }

  // The contained value, or what `f` returns when absent.
  pub fn or_else(self: Self, f: () -> Option<T>) -> Option<T> {
    ret match self {
      Option::Some(v) => Option::Some(v),
      Option::None => f(),
    }
  }

  // Keeps the value when `f` says so, and drops it otherwise.
  pub fn filter(self: Self, f: (&T) -> bool) -> Option<T> {
    ret match self {
      Option::Some(v) => if f(&v) {
        Option::Some(v)
      } else {
        Option::None
      },
      Option::None => Option::None,
    }
  }

  // Applies `f` to the contained value; `None` stays absent. The
  // result type is the call's to spell (`o.map::<U>(f)`); it appears
  // nowhere in the receiver.
  pub fn map<U>(self: Self, f: (T) -> U) -> Option<U> {
    ret match self {
      Option::Some(v) => Option::Some(f(v)),
      Option::None => Option::None,
    }
  }

  // Chains an option-returning `f`; `None` stays absent.
  pub fn and_then<U>(self: Self, f: (T) -> Option<U>) -> Option<U> {
    ret match self {
      Option::Some(v) => f(v),
      Option::None => Option::None,
    }
  }

  // The contained value as `Ok`, or `err` as `Err`.
  pub fn ok_or<E>(self: Self, err: E) -> Result<T, E> {
    ret match self {
      Option::Some(v) => Result::Ok(v),
      Option::None => Result::Err(err),
    }
  }
}

// `==` compares shapes and payloads: two `Some`s are equal when their
// payloads are, two `None`s are equal, and mixed shapes are not.
// Whether `T` compares is checked where the generic body is
// instantiated.
impl<T: PartialEq> PartialEq for Option<T> {
  fn eq(self: &Self, other: &Self) -> bool {
    ret match *self {
      Option::Some(a) => match *other {
        Option::Some(b) => *a == *b,
        Option::None => false,
      },
      Option::None => match *other {
        Option::Some(_) => false,
        Option::None => true,
      },
    }
  }
}

impl<T: Eq> Eq for Option<T> {
}
