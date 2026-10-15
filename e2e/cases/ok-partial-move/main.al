// Partial moves out of an owned value, the shape a compiler uses to
// hand fields back from a consumed node.
struct Pair {
  a: String,
  b: String,
}

impl Pair {
  fn both(self: Pair) -> String {
    mut out := self.a
    out.push_str(self.b.as_str())
    ret out
  }
}

fn main() -> i32 {
  p := Pair { a: format("x"), b: format("y") }
  if p.both().as_str() != "xy" { ret 1 }
  ret 0
}
