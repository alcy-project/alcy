// A mutable receiver written back across comp calls advances state.
enum Maybe<T> {
  Some(T),
  None,
}

spec Cur<T> {
  fn next(mut self: &mut Self) -> Maybe<T>;
}

struct Counter {
  pos: i32,
  end: i32,
}

impl Cur < i32 > for Counter {
  fn next(mut self: &mut Self) -> Maybe<i32> {
    if self.pos >= self.end {
      ret Maybe::None
    }
    v := self.pos
    self.pos += 1
    ret Maybe::Some(v)
  }
}

const V: i32 := comp {
  mut c := Counter { pos: 0, end: 5 }
  mut s := 0
  loop {
    match c.next() {
      Maybe::Some(v) => {
        s += v
      }
      Maybe::None => break,
    }
  }
  s
}

fn main() -> i32 {
  if V != 10 {
    ret 1
  }
  ret 0
}
