// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, core member.
//
// This file ships inside the compiler binary and is injected as a
// prelude module into every compilation, so its public items need no
// imports. It declares compiler-provided intrinsics only; everything
// else in core arrives as ordinary library code later.

pub intrinsic fn memcopy(dst: &mut u8, src: &u8, n: usize);

pub intrinsic fn panic(msg: str) -> !;

// Heap allocation. `align` must be a power of two; a zero `size`
// still yields a distinct freeable pointer. The returned reference
// uniquely owns uninitialized bytes and must be released with
// `dealloc` using the same `size` and `align`.
pub intrinsic fn alloc(size: usize, align: usize) -> &mut u8;

pub intrinsic fn dealloc(ptr: &mut u8, size: usize, align: usize);

intrinsic fn sys_write(fd: i32, buf: str);

pub fn print(msg: str) {
  sys_write(1, msg)
}

pub fn println(msg: str) {
  sys_write(1, msg)
  sys_write(1, "\n")
}

pub intrinsic fn str_len(s: str) -> usize;

pub intrinsic fn str_byte(s: str, i: usize) -> u8;

pub intrinsic fn str_slice(s: str, start: usize, end: usize) -> str;

pub struct WriteOutcome { written: usize, total: usize }

intrinsic fn str_from_parts(ptr: &u8, len: usize) -> str;

pub struct String { buf: [u8; 256], len: usize }

impl String {
  pub fn new() -> String {
    ret String { buf: [0u8; 256], len: 0 }
  }

  pub fn len(self: &Self) -> usize {
    ret self.len
  }

  pub fn push(mut self: &mut Self, b: u8) {
    if self.len >= 256 {
      panic("String is full")
    }
    self.buf[self.len] = b
    self.len = self.len + 1
  }

  pub fn as_str(self: &Self) -> str {
    ret str_from_parts(&self.buf[0], self.len)
  }
}

// Formats `args` into an owned string; see docs/spec/fmt.md.
// Outputs longer than the internal buffer abort rather than
// truncating silently.
pub fn format(comp fmt: str, args: ()) -> String {
  mut out := String::new()
  result := write(fmt, &mut out.buf, args)
  if result.total != result.written {
    panic("format output truncated")
  }
  out.len = result.written
  ret out
}

// Formats `args` into `buf` by compile-time expansion; see
// docs/spec/fmt.md. Calls check and expand through compiler support
// (like the print intrinsics); the body never executes, and reaching
// it aborts. The `[u8; 0]` and `()` parameter types are placeholders:
// calls accept any byte-array size and any tuple arity through custom
// checking, since the language cannot name them yet.
pub fn write(comp fmt: str, buf: &mut [u8; 0], args: ()) -> WriteOutcome {
  panic("fmt::write must expand")
}

// A value that may be absent. `None` carries no payload, so
// `Option<T>` is copyable for every `T`.
//
// The `?` operator propagates any non-`Some` variant of an identical
// enclosing return type; see docs/adr/0009.
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
