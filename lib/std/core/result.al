// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: the result type.

// A computation that either succeeded with a `T` or failed with an
// `E`. `main` may return `Result<(), E>`; the entry thunk maps `Ok` to
// exit code 0 and aborts on `Err`.
pub enum Result<T, E> {
  Ok(T),
  Err(E),
}

impl<T, E> Result<T, E> {
  pub fn is_ok(self: Self) -> bool {
    ret match self {
      Result::Ok(_) => true,
      Result::Err(_) => false,
    }
  }

  pub fn is_err(self: Self) -> bool {
    ret match self {
      Result::Ok(_) => false,
      Result::Err(_) => true,
    }
  }

  // Panics on `Err`, discarding the payload. Prefer `is_ok` or `?`
  // where failure is expected rather than exceptional.
  pub fn unwrap(self: Self) -> T {
    ret match self {
      Result::Ok(v) => v,
      Result::Err(_) => panic("called `Result::unwrap` on an `Err` value"),
    }
  }

  pub fn expect(self: Self, msg: str) -> T {
    ret match self {
      Result::Ok(v) => v,
      Result::Err(_) => panic(msg),
    }
  }

  // Yields the contained value or `default` on failure.
  pub fn or(self: Self, default: T) -> T {
    ret match self {
      Result::Ok(v) => v,
      Result::Err(_) => default,
    }
  }
}

