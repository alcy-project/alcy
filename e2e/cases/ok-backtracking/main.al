// A parser that tries a production and rolls back on failure.
struct Parser {
  kinds: Vec<i32>,
  pos: usize,
}

impl Parser {
  fn at(self: &Self) -> i32 {
    if self.pos >= self.kinds.len() {
      ret 0
    }
    ret *self.kinds.at(self.pos).unwrap()
  }

  fn try_ab(mut self: &mut Self) -> bool {
    mark := self.pos
    if self.at() != 1 {
      ret false
    }
    self.pos = self.pos + 1
    if self.at() != 2 {
      self.pos = mark
      ret false
    }
    self.pos = self.pos + 1
    ret true
  }
}

fn main() -> i32 {
  mut p := Parser { kinds: Vec::<i32>::new(), pos: 0 }
  p.kinds.push(1)
  p.kinds.push(3)
  if p.try_ab() { ret 1 }
  if p.pos != 0 { ret 2 }
  mut q := Parser { kinds: Vec::<i32>::new(), pos: 0 }
  q.kinds.push(1)
  q.kinds.push(2)
  if !q.try_ab() { ret 3 }
  if q.pos != 2 { ret 4 }
  ret 0
}
