// A recursive expression reader: `?` propagates a missing operand
// out of a method that answers `Option`.
struct Parser {
  src: String,
  pos: usize,
}

impl Parser {
  fn new(src: str) -> Parser {
    ret Parser { src: String::from(src), pos: 0 }
  }

  fn atom(mut self: &mut Self) -> Option<i32> {
    if self.pos >= self.src.len() {
      ret Option::None
    }
    b := self.src.byte(self.pos)
    if b < 0x30u8 || b > 0x39u8 {
      ret Option::None
    }
    mut value := 0
    while self.pos < self.src.len() {
      c := self.src.byte(self.pos)
      if c < 0x30u8 || c > 0x39u8 { break }
      value = value * 10 + ((c - 0x30u8) as i32)
      self.pos = self.pos + 1
    }
    ret Option::Some(value)
  }

  fn sum(mut self: &mut Self) -> Option<i32> {
    mut total := self.atom()?
    while self.pos < self.src.len() {
      if self.src.byte(self.pos) != 0x2bu8 { break }
      self.pos = self.pos + 1
      total = total + self.atom()?
    }
    ret Option::Some(total)
  }
}

fn main() -> i32 {
  mut p := Parser::new("12+30+5")
  match p.sum() {
    Option::Some(v) => {
      if v != 47 { ret 1 }
    }
    Option::None => { ret 2 }
  }
  mut bad := Parser::new("12+")
  if bad.sum().is_some() { ret 3 }
  ret 0
}
