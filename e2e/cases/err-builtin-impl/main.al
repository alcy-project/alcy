// A builtin type belongs to the staged suite, so a program cannot add
// methods to one (ADR-0090).
impl str {
  fn shout(self: &Self) -> i32 {
    ret 1
  }
}

fn main() -> i32 {
  ret 0
}
