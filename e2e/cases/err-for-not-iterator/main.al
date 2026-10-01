// A cursor with an inherent `next` is not an `Iterator`: the generated
// `next` call resolves through specs only, so `for` rejects this head.
struct Cursor {
  index: i32,
}

impl Cursor {
  fn into_iter(self: Self) -> Cursor {
    ret self
  }

  fn next(mut self: &mut Self) -> Option<i32> {
    if self.index >= 3 {
      ret Option::None
    }
    v := self.index
    self.index = self.index + 1
    ret Option::Some(v)
  }
}

fn main() {
  c := Cursor { index: 0 }
  for v in c {}
}
