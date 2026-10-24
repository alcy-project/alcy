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
  // Consulted through a shared borrow, so the result is still there
  // to ask again.
  pub fn is_ok(self: &Self) -> bool {
    ret match *self {
      Result::Ok(_) => true,
      Result::Err(_) => false,
    }
  }

  pub fn is_err(self: &Self) -> bool {
    ret match *self {
      Result::Ok(_) => false,
      Result::Err(_) => true,
    }
  }

  // The contained value, or a panic whose message is `msg`. Prefer
  // `is_ok` or `?` where failure is expected rather than exceptional.
  pub fn or_panic_with_message(self: Self, msg: str) -> T {
    ret match self {
      Result::Ok(v) => v,
      Result::Err(_) => panic(msg),
    }
  }

  // The contained value, or a panic whose message the error builds.
  // `f` runs only on `Err`.
  pub fn or_panic_with(self: Self, f: (E) -> str) -> T {
    ret match self {
      Result::Ok(v) => v,
      Result::Err(e) => panic(f(e)),
    }
  }

  // Yields the contained value or `default` on failure.
  pub fn or(self: Self, default: T) -> T {
    ret match self {
      Result::Ok(v) => v,
      Result::Err(_) => default,
    }
  }

  // The contained value, or what `f` returns on failure. `f` runs
  // only on `Err`.
  pub fn or_else(self: Self, f: () -> T) -> T {
    ret match self {
      Result::Ok(v) => v,
      Result::Err(_) => f(),
    }
  }
}

// `==` compares the shapes and then the matching payloads: `Ok` with
// `Ok` through `T`, `Err` with `Err` through `E`, and mixed shapes are
// not equal. Whether the payload compares is checked where the generic
// body is instantiated.
impl<T: PartialEq, E: PartialEq> PartialEq for Result<T, E> {
  fn eq(self: &Self, other: &Self) -> bool {
    ret match *self {
      Result::Ok(a) => match *other {
        Result::Ok(b) => *a == *b,
        Result::Err(_) => false,
      },
      Result::Err(a) => match *other {
        Result::Ok(_) => false,
        Result::Err(b) => *a == *b,
      },
    }
  }
}

impl<T: Eq, E: Eq> Eq for Result<T, E> {
}
