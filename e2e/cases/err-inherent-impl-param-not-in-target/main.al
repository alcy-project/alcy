// An inherent block's parameters bind through the target type too, so
// the same declaration rule covers both kinds of block.

struct P {
  base: i32,
}

impl<U> P {
  fn echo(self: &P, other: &U) -> i32 {
    ret self.base
  }
}

fn main() -> i32 {
  ret 0
}
