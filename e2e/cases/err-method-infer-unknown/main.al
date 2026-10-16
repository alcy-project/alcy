// A parameter no argument pins waits for a spelling the method path
// does not have.
struct Maker {
  base: i32,
}

impl Maker {
  fn make<U>(self: &Self, x: i32) -> U {
    ret self.base
  }
}

fn main() -> i32 {
  m := Maker { base: 1 }
  ret m.make(2)
}
