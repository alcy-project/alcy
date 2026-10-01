// A countdown cursor and a cursor whose inherent `next` disagrees with
// its `Iterator` implementation, so the `for` rule's dispatch is
// observable at run time.
pub struct Countdown {
  remaining: i32,
}

impl Countdown {
  pub fn new(from: i32) -> Countdown {
    ret Countdown { remaining: from }
  }

  // `into_iter` is an ordinary method; `Countdown` is Copy, so the
  // loop works on a copy and the original keeps its position.
  pub fn into_iter(self: Self) -> Countdown {
    ret self
  }
}

impl Iterator<i32> for Countdown {
  fn next(mut self: &mut Self) -> Option<i32> {
    if self.remaining <= 0 {
      ret Option::None
    }
    v := self.remaining
    self.remaining = self.remaining - 1
    ret Option::Some(v)
  }
}

pub struct Wrapped {
  remaining: i32,
}

impl Wrapped {
  pub fn new(from: i32) -> Wrapped {
    ret Wrapped { remaining: from }
  }

  pub fn into_iter(self: Self) -> Wrapped {
    ret self
  }

  // Explicit calls resolve here; the `for` desugar must resolve the
  // generated `next` through the spec instead.
  pub fn next(mut self: &mut Self) -> Option<i32> {
    ret Option::Some(-100)
  }
}

impl Iterator<i32> for Wrapped {
  fn next(mut self: &mut Self) -> Option<i32> {
    if self.remaining <= 0 {
      ret Option::None
    }
    v := self.remaining
    self.remaining = self.remaining - 1
    ret Option::Some(v)
  }
}
