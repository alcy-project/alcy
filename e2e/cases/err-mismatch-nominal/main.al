// A mismatch between two nominals names both, not their shared kind.
struct A {
  x: i32,
}

struct B {
  y: i32,
}

fn main() {
  a: A := B { y: 1 }
}
