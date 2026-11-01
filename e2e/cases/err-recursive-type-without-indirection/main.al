// A type that holds itself has no size, so the walk that asks a shape's
// size stops at the cycle and the type checks refuse it (EC007) - it does
// not follow the cycle until the compiler's own stack ends.

struct A {
  b: B,
}

struct B {
  a: A,
}

// A parameter asks whether a value of the type may be moved, and that
// walk follows the fields too.
fn take(a: A) {}

fn main() -> i32 {
  ret 0
}
