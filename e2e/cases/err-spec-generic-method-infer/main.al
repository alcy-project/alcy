// A method type parameter no argument pins has nothing to infer from:
// the call has to spell it.
spec Make {
  fn make<T>(self: &Self) -> T;
}

struct W {
  base: i32,
}

impl Make for W {
  fn make<U>(self: &Self) -> U {
    ret self.base
  }
}

fn main() -> i32 {
  w := W { base: 7 }
  ret w.make()
}
