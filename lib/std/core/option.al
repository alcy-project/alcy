// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: the option type.

pub enum Option<T> {
  Some(T),
  None,
}

impl<T> Option<T> {
  pub fn is_some(self: Self) -> bool {
    ret match self {
      Option::Some(_) => true,
      Option::None => false,
    }
  }

  pub fn is_none(self: Self) -> bool {
    ret match self {
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
}

