// The for pattern type-checks against the item the cursor yields.
struct Cursor {
  remaining: i32,
}

impl Cursor {
  fn into_iter(self: Self) -> Cursor {
    ret self
  }
}

impl Iterator<i32> for Cursor {
  fn next(mut self: &mut Self) -> Option<i32> {
    if self.remaining <= 0 {
      ret Option::None
    }
    v := self.remaining
    self.remaining = self.remaining - 1
    ret Option::Some(v)
  }
}

fn main() {
  c := Cursor { remaining: 3 }
  for (a, b) in c {}
}
