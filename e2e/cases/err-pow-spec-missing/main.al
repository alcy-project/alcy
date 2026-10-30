// A nominal type with no `Pow` implementation has no `**`: the
// operator resolves through the sealed spec, not the backend.
struct Point {
  x: i32,
}

fn main() -> i32 {
  a := Point { x: 2 }
  b := Point { x: 3 }
  c := a ** b
  ret c.x
}
