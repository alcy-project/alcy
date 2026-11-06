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

test {
  test an_ok_is_ok() {
    ok: Result<i32, str> := Result::Ok(7)
    assert(ok.is_ok(), "an Ok is a success")
  }

  test an_ok_is_not_err() {
    ok: Result<i32, str> := Result::Ok(7)
    assert(!ok.is_err(), "an Ok is not a failure")
  }

  test an_err_is_err() {
    bad: Result<i32, str> := Result::Err("bad")
    assert(bad.is_err(), "an Err is a failure")
  }

  test an_err_is_not_ok() {
    bad: Result<i32, str> := Result::Err("bad")
    assert(!bad.is_ok(), "an Err is not a success")
  }

  test or_on_an_ok_yields_the_value() {
    ok: Result<i32, str> := Result::Ok(7)
    assert(ok.or(9) == 7, "a success keeps its value")
  }

  test or_on_an_err_yields_the_default() {
    bad: Result<i32, str> := Result::Err("bad")
    assert(bad.or(9) == 9, "the default stands in for a failure")
  }

  test or_else_on_an_ok_yields_the_value() {
    ok: Result<i32, str> := Result::Ok(7)
    assert(ok.or_else(() -> 9) == 7, "a success keeps its value")
  }

  test or_else_on_an_err_builds_the_default() {
    bad: Result<i32, str> := Result::Err("bad")
    assert(bad.or_else(() -> 9) == 9, "the fallback runs for a failure")
  }

  test or_panic_with_message_yields_the_value_of_an_ok() {
    ok: Result<i32, str> := Result::Ok(7)
    assert(ok.or_panic_with_message("a success has a value") == 7, "a success carries its value")
  }

  test or_panic_with_yields_the_value_of_an_ok() {
    ok: Result<i32, str> := Result::Ok(7)
    assert(ok.or_panic_with((e: str) -> e) == 7, "the message builder never runs for a success")
  }

  test two_oks_with_equal_values_are_equal() {
    a: Result<i32, str> := Result::Ok(7)
    b: Result<i32, str> := Result::Ok(7)
    assert(a == b, "equal payloads make equal results")
  }

  test oks_with_different_values_are_unequal() {
    a: Result<i32, str> := Result::Ok(7)
    b: Result<i32, str> := Result::Ok(8)
    assert(a != b, "different payloads make unequal results")
  }

  test two_errs_with_equal_errors_are_equal() {
    a: Result<i32, str> := Result::Err("bad")
    b: Result<i32, str> := Result::Err("bad")
    assert(a == b, "equal errors make equal results")
  }

  test errs_with_different_errors_are_unequal() {
    a: Result<i32, str> := Result::Err("bad")
    b: Result<i32, str> := Result::Err("worse")
    assert(a != b, "different errors make unequal results")
  }

  test a_success_and_a_failure_are_unequal() {
    ok: Result<i32, str> := Result::Ok(7)
    bad: Result<i32, str> := Result::Err("bad")
    assert(ok != bad, "a success is not a failure")
    assert(bad != ok, "and a failure is not a success")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
