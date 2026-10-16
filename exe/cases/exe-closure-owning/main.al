// A closure owns the environment it moved its captures into: the value
// ends them, whether it is the binding or the slot a coerced literal
// keeps in the frame. The sanitized run is what checks the frees.

fn apply(f: () -> i32) -> i32 {
  ret f()
}

fn main() -> i32 {
  mut t := String::new()
  t.push_str("hello")
  greet := [t]() -> {
    println("{}", t.len())
    ret t.len()
  }
  if greet() != 5 {
    ret 1
  }

  mut u := String::new()
  u.push_str("world!")
  // The view borrows the environment the value owns; the enclosing
  // scope ends it.
  if apply(
    [u]() ->
      {
        ret u.len() as i32
      }
  ) != 6 {
    ret 2
  }
  ret 0
}
