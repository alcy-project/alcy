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

  // Applies `f` to the contained value, leaving a failure alone.
  pub fn map<U>(self: Self, f: (T) -> U) -> Result<U, E> {
    ret match self {
      Result::Ok(v) => Result::Ok(f(v)),
      Result::Err(e) => Result::Err(e),
    }
  }

  // Applies `f` to the failure, leaving a success alone.
  pub fn map_err<F>(self: Self, f: (E) -> F) -> Result<T, F> {
    ret match self {
      Result::Ok(v) => Result::Ok(v),
      Result::Err(e) => Result::Err(f(e)),
    }
  }

  // Chains a `Result<U, E>`-returning `f`; an error stays an error.
  pub fn and_then<U>(self: Self, f: (T) -> Result < U, E >) -> Result<U, E> {
    ret match self {
      Result::Ok(v) => f(v),
      Result::Err(e) => Result::Err(e),
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

#[when(testing)] {
  #[test] fn ok_is_ok() {
    ok: Result<i32, str> := Result::Ok(7)
    assert(ok.is_ok(), "an Ok is a success")
  }

  #[test] fn ok_is_not_err() {
    ok: Result<i32, str> := Result::Ok(7)
    assert(!ok.is_err(), "an Ok is not a failure")
  }

  #[test] fn err_is_err() {
    bad: Result<i32, str> := Result::Err("bad")
    assert(bad.is_err(), "an Err is a failure")
  }

  #[test] fn err_is_not_ok() {
    bad: Result<i32, str> := Result::Err("bad")
    assert(!bad.is_ok(), "an Err is not a success")
  }

  #[test] fn or_on_an_ok_yields_the_value() {
    ok: Result<i32, str> := Result::Ok(7)
    assert(ok.or(9) == 7, "a success keeps its value")
  }

  #[test] fn or_on_an_err_yields_the_default() {
    bad: Result<i32, str> := Result::Err("bad")
    assert(bad.or(9) == 9, "the default stands in for a failure")
  }

  #[test] fn or_else_on_an_ok_yields_the_value() {
    ok: Result<i32, str> := Result::Ok(7)
    assert(ok.or_else(() -> 9) == 7, "a success keeps its value")
  }

  #[test] fn or_else_on_an_err_builds_the_default() {
    bad: Result<i32, str> := Result::Err("bad")
    assert(bad.or_else(() -> 9) == 9, "the fallback runs for a failure")
  }

  #[test] fn or_panic_with_message_yields_the_value_of_an_ok() {
    ok: Result<i32, str> := Result::Ok(7)
    assert(ok.or_panic_with_message("a success has a value") == 7, "a success carries its value")
  }

  #[test] fn or_panic_with_yields_the_value_of_an_ok() {
    ok: Result<i32, str> := Result::Ok(7)
    assert(ok.or_panic_with((e: str) -> e) == 7, "the message builder never runs for a success")
  }

  #[test] fn two_oks_with_equal_values_are_equal() {
    a: Result<i32, str> := Result::Ok(7)
    b: Result<i32, str> := Result::Ok(7)
    assert(a == b, "equal payloads make equal results")
  }

  #[test] fn oks_with_different_values_are_unequal() {
    a: Result<i32, str> := Result::Ok(7)
    b: Result<i32, str> := Result::Ok(8)
    assert(a != b, "different payloads make unequal results")
  }

  #[test] fn map_applies_f_to_the_value_of_an_ok() {
    ok: Result<i32, str> := Result::Ok(7)
    mapped := ok.map((v: i32) -> v + 1)
    want: Result<i32, str> := Result::Ok(8)
    assert(mapped == want, "the value reaches f")
  }

  #[test] fn map_leaves_an_err_alone() {
    bad: Result<i32, str> := Result::Err("bad")
    mapped := bad.map((v: i32) -> v + 1)
    want: Result<i32, str> := Result::Err("bad")
    assert(mapped == want, "a failure stays a failure")
  }

  #[test] fn map_err_applies_f_to_the_error_of_an_err() {
    bad: Result<i32, str> := Result::Err("bad")
    erring := bad.map_err((e: str) -> str_len(e))
    three: usize := 3
    want: Result<i32, usize> := Result::Err(three)
    assert(erring == want, "the error reaches f")
  }

  #[test] fn map_err_leaves_an_ok_alone() {
    ok: Result<i32, str> := Result::Ok(7)
    erring := ok.map_err((e: str) -> str_len(e))
    want: Result<i32, usize> := Result::Ok(7)
    assert(erring == want, "a success stays a success")
  }

  #[test] fn and_then_chains_a_result_returning_f() {
    ok: Result<i32, str> := Result::Ok(7)
    half: (i32) -> Result<i32, str> := (v: i32) -> match v {
      0 => Result::Err("zero"),
      _ => Result::Ok(v / 2),
    }
    chained := ok.and_then::<i32>(half)
    want: Result<i32, str> := Result::Ok(3)
    assert(chained == want, "the value reaches f")
    zero: Result<i32, str> := Result::Ok(0)
    blocked := zero.and_then::<i32>(half)
    want_err: Result<i32, str> := Result::Err("zero")
    assert(blocked == want_err, "f's error stands in for the chain")
  }

  #[test] fn and_then_leaves_an_err_alone() {
    bad: Result<i32, str> := Result::Err("bad")
    chained := bad.and_then::<i32>((v: i32) -> Result::Ok(v + 1))
    want: Result<i32, str> := Result::Err("bad")
    assert(chained == want, "a failure stays a failure")
  }

  #[test] fn two_errs_with_equal_errors_are_equal() {
    a: Result<i32, str> := Result::Err("bad")
    b: Result<i32, str> := Result::Err("bad")
    assert(a == b, "equal errors make equal results")
  }

  #[test] fn errs_with_different_errors_are_unequal() {
    a: Result<i32, str> := Result::Err("bad")
    b: Result<i32, str> := Result::Err("worse")
    assert(a != b, "different errors make unequal results")
  }

  #[test] fn success_and_a_failure_are_unequal() {
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
