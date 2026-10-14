// Binding a non-Copy payload by value out of a borrow is still a
// move out of borrowed content, so it stays refused.

enum O<T> { A(T), B }

fn take(self: &O<String>) -> String {
  ret match *self { O::A(v) => v, O::B => format("") }
}

fn main() -> i32 {
  o := O::A(format("x"))
  if take(&o).len() != 1 { ret 1 }
  ret 0
}
