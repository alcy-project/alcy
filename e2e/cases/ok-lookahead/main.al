// A scanner with two-token lookahead: `peek` reads ahead without
// moving, `bump` consumes.
struct Lexer {
  kinds: Vec<i32>,
  pos: usize,
}

impl Lexer {
  fn peek(self: &Self, ahead: usize) -> i32 {
    at := self.pos + ahead
    if at >= self.kinds.len() {
      ret 0
    }
    ret *self.kinds.at(at).or_panic_with_message("expected a value")
  }

  fn bump(mut self: &mut Self) -> i32 {
    k := self.peek(0)
    if self.pos < self.kinds.len() {
      self.pos = self.pos + 1
    }
    ret k
  }
}

fn main() -> i32 {
  mut lx := Lexer { kinds: Vec::<i32>::new(), pos: 0 }
  lx.kinds.push(1)
  lx.kinds.push(2)
  lx.kinds.push(3)
  if lx.peek(1) != 2 {
    ret 1
  }
  if lx.peek(5) != 0 {
    ret 2
  }
  if lx.bump() != 1 {
    ret 3
  }
  if lx.peek(0) != 2 {
    ret 4
  }
  ret 0
}
