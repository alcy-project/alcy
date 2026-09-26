// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `core` package.
//
// This file is the package's root module. Its `pub` items are the
// package's implicit surface: a program that depends on
// `alcy/std/core` sees them without a `use` declaration.
//
// Core declares compiler-provided intrinsics and the types that need
// nothing else. It is the one package with no dependencies, and every
// other package of the suite may use it without declaring that. The
// heap and the values that live on it belong to `alloc`; see
// docs/adr/0016.

pub intrinsic fn memcopy(dst: &mut u8, src: &u8, n: usize);

pub intrinsic fn panic(msg: str) -> !;

intrinsic fn sys_write(fd: i32, buf: str);

pub fn print(msg: str) {
  sys_write(1, msg)
}

pub fn println(msg: str) {
  sys_write(1, msg)
  sys_write(1, "\n")
}

pub intrinsic fn size_of<T>() -> usize;

pub intrinsic fn align_of<T>() -> usize;

pub intrinsic fn str_len(s: str) -> usize;

pub intrinsic fn str_byte(s: str, i: usize) -> u8;

pub intrinsic fn str_slice(s: str, start: usize, end: usize) -> str;

pub intrinsic fn str_from_parts(ptr: &u8, len: usize) -> str;

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
