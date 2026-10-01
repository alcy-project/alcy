// Cursors implement core's `Iterator`; the call sites never import it,
// because the prelude re-exports the spec to every module.
pub struct Countdown {
  remaining: i32,
}

impl Countdown {
  pub fn new(from: i32) -> Countdown {
    ret Countdown { remaining: from }
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

// A view over a fixed array, stepped by an index.
pub struct Bytes {
  data: [u8; 3],
  index: i32,
}

pub fn bytes(data: [u8; 3]) -> Bytes {
  ret Bytes { data: data, index: 0 }
}

impl Iterator<u8> for Bytes {
  fn next(mut self: &mut Self) -> Option<u8> {
    if self.index >= 3 {
      ret Option::None
    }
    v := self.data[self.index]
    self.index = self.index + 1
    ret Option::Some(v)
  }
}
