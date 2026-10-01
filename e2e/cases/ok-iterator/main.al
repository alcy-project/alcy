// `Iterator` comes from core's prelude: implementing it needs no
// import, and a cursor keeps its position across `next` calls.
struct Counter {
  next_value: i32,
  limit: i32,
}

impl Iterator<i32> for Counter {
  fn next(mut self: &mut Self) -> Option<i32> {
    if self.next_value > self.limit {
      ret Option::None
    }
    v := self.next_value
    self.next_value = self.next_value + 1
    ret Option::Some(v)
  }
}

fn main() -> i32 {
  mut c := Counter { next_value: 1, limit: 3 }
  mut sum := 0
  loop {
    match c.next() {
      Option::Some(v) => {
        sum = sum + v
      }
      Option::None => break,
    }
  }
  if sum != 6 { ret 1 }
  // An exhausted iterator stays exhausted.
  if c.next().is_some() { ret 2 }
  ret 0
}
