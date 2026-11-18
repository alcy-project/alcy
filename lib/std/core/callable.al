// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: calling as a capability.

// The capability behind the `fn(...)` spellings: a value
// whose type carries the signature `P -> R` is callable, with `P` the
// parameters as one tuple. Every closure nominal implements it by
// construction - the implementation is the code the lowering registers
// for the literal - and `fn(A, B) -> R` in a signature names this bound
// for an anonymous parameter. The spec is sealed to the suite
// : the compiler owns the implementations, so no user type
// opts in by writing one.
pub spec Fn<P, R> {
  fn call(self: &Self, args: P) -> R;
}

// A module's own tests: 'alcy test' splices the block into a driver and
// runs each entry as a process of its own.
test {
  test a_named_function_satisfies_the_signature() {
    assert(apply(double, 3) == 6, "the body calls through the bound")
  }

  test a_closure_literal_satisfies_the_signature() {
    assert(apply((v: i32) -> v + 1i32, 2) == 3, "the literal's own code runs")
  }

  test a_signature_of_no_parameters_is_callable() {
    assert(run(() -> 5i32) == 10, "each call yields a value of its own")
  }

  test two_parameters_pass_as_one_tuple() {
    assert(
      combine((x: i32, y: i32) -> x * y, 3, 4) == 12,
      "the arguments reach the two parameters in order",
    )
  }

  test the_bound_reads_the_result_type() {
    assert(apply((v: i32) -> 0i32 - v, 7) == 0i32 - 7i32, "a negative result arrives intact")
  }

  fn apply<F: Fn<(i32,), i32>>(f: F, x: i32) -> i32 {
    ret f(x)
  }

  fn combine<F: Fn<(i32, i32), i32>>(f: F, a: i32, b: i32) -> i32 {
    ret f(a, b)
  }

  fn run<F: Fn<(), i32>>(f: F) -> i32 {
    ret f() + f()
  }

  fn double(v: i32) -> i32 {
    ret v * 2i32
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
