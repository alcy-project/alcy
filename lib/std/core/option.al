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

  // The contained value, or a panic whose message is `msg`. Prefer
  // `is_some` or `?` where absence is expected rather than
  // exceptional.
  pub fn or_panic_with_message(self: Self, msg: str) -> T {
    ret match self {
      Option::Some(v) => v,
      Option::None => panic(msg),
    }
  }

  // The contained value, or a panic whose message `f` builds. `f`
  // runs only on `None`.
  pub fn or_panic_with(self: Self, f: () -> str) -> T {
    ret match self {
      Option::Some(v) => v,
      Option::None => panic(f()),
    }
  }

  // Yields the contained value or `default` when absent.
  pub fn or(self: Self, default: T) -> T {
    ret match self {
      Option::Some(v) => v,
      Option::None => default,
    }
  }

  // The contained value, or what `f` returns when absent. `f` runs
  // only on `None`.
  pub fn or_else(self: Self, f: () -> T) -> T {
    ret match self {
      Option::Some(v) => v,
      Option::None => f(),
    }
  }

  // The option, or what `f` returns when absent. `f` runs only on
  // `None`.
  pub fn or_option(self: Self, f: () -> Option<T>) -> Option<T> {
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
      }
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

#[when(testing)] {
  #[test] fn a_some_is_some() {
    assert(Option::Some(3).is_some(), "a Some holds a value")
  }

  #[test] fn a_some_is_not_none() {
    assert(!Option::Some(3).is_none(), "a Some is not the absence")
  }

  #[test] fn an_absence_is_none() {
    assert(absent().is_none(), "a None is the absence")
  }

  #[test] fn an_absence_is_not_some() {
    assert(!absent().is_some(), "a None holds no value")
  }

  #[test] fn or_yields_the_contained_value() {
    assert(Option::Some(3).or(9) == 3, "a present value beats the default")
  }

  #[test] fn or_yields_the_default_when_absent() {
    assert(absent().or(9) == 9, "the default stands in for an absence")
  }

  #[test] fn or_else_yields_the_contained_value() {
    assert(Option::Some(3).or_else(() -> 9) == 3, "a present value beats the fallback")
  }

  #[test] fn or_else_builds_the_value_when_absent() {
    assert(absent().or_else(() -> 9) == 9, "the fallback runs for an absence")
  }

  #[test] fn or_option_keeps_a_present_option() {
    kept := Option::Some(3).or_option(() -> Option::Some(9))
    assert(
      kept.or_panic_with_message("a present option stays") == 3,
      "a present option is not replaced",
    )
  }

  #[test] fn or_option_falls_back_to_the_next_option() {
    taken := absent().or_option(() -> Option::Some(9))
    assert(
      taken.or_panic_with_message("the fallback is present") == 9,
      "the fallback supplies the value",
    )
  }

  #[test] fn or_option_can_stay_absent() {
    assert(
      absent().or_option(() -> absent()).is_none(),
      "an absent fallback leaves the option absent",
    )
  }

  #[test] fn filter_keeps_a_value_the_predicate_accepts() {
    kept := Option::Some(4).filter((x: &i32) -> *x == 4)
    assert(kept.or_panic_with_message("the value is kept") == 4, "an accepted value survives")
  }

  #[test] fn filter_drops_a_value_the_predicate_rejects() {
    assert(Option::Some(4).filter((x: &i32) -> *x == 5).is_none(), "a rejected value is gone")
  }

  #[test] fn filter_leaves_an_absence_absent() {
    assert(absent().filter((x: &i32) -> *x == 4).is_none(), "there is nothing to keep")
  }

  #[test] fn map_transforms_the_contained_value() {
    mapped := Option::Some(3).map::<i32>((x: i32) -> x * 10)
    assert(
      mapped.or_panic_with_message("the value is mapped") == 30,
      "the payload goes through the mapping",
    )
  }

  #[test] fn map_leaves_an_absence_absent() {
    assert(absent().map::<i32>((x: i32) -> x * 10).is_none(), "there is nothing to map")
  }

  #[test] fn map_can_change_the_payload_type() {
    mapped := Option::Some(3).map::<bool>((x: i32) -> x > 0)
    assert(
      mapped.or_panic_with_message("the value is mapped"),
      "the result carries the new payload",
    )
  }

  #[test] fn and_then_chains_a_present_value() {
    chained := Option::Some(3).and_then::<i32>((x: i32) -> Option::Some(x + 1))
    assert(
      chained.or_panic_with_message("the chain stays present") == 4,
      "the second option decides the value",
    )
  }

  #[test] fn and_then_leaves_an_absence_absent() {
    assert(
      absent().and_then::<i32>((x: i32) -> Option::Some(x + 1)).is_none(),
      "there is nothing to chain",
    )
  }

  #[test] fn and_then_can_drop_a_present_value() {
    dropped := Option::Some(3).and_then::<i32>((x: i32) -> below_one(x))
    assert(dropped.is_none(), "a present value can end the chain absent")
  }

  #[test] fn ok_or_wraps_a_present_value_as_ok() {
    wrapped := Option::Some(3).ok_or("absent")
    assert(wrapped.is_ok(), "a present value is a success")
    assert(wrapped.or_panic_with_message("a present value is a success") == 3, "and keeps it")
  }

  #[test] fn ok_or_wraps_an_absence_as_err() {
    wrapped := absent().ok_or("absent")
    assert(wrapped.is_err(), "an absence is a failure")
  }

  #[test] fn ok_or_keeps_the_error_it_was_given() {
    wrapped := absent().ok_or("absent")
    expected: Result<i32, str> := Result::Err("absent")
    assert(wrapped == expected, "the error carried over unchanged")
  }

  #[test] fn options_with_equal_payloads_are_equal() {
    assert(Option::Some(3) == Option::Some(3), "equal payloads make equal options")
  }

  #[test] fn options_with_different_payloads_are_unequal() {
    assert(Option::Some(3) != Option::Some(4), "different payloads make unequal options")
  }

  #[test] fn two_absences_are_equal() {
    assert(absent() == absent(), "two absences are equal")
  }

  #[test] fn a_value_and_an_absence_are_unequal() {
    assert(Option::Some(3) != absent(), "a present option is not the absence")
    assert(!(Option::Some(3) == absent()), "and equality agrees")
  }

  fn absent() -> Option<i32> {
    ret Option::None
  }

  fn below_one(x: i32) -> Option<i32> {
    if x < 1 {
      ret Option::Some(x)
    }
    ret Option::None
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
