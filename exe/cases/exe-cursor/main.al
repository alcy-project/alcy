// A cursor struct a scanner would hold: state behind `&mut self` and
// a read-only query behind `&self`.
struct Cursor {
  src: String,
  pos: usize,
}

impl Cursor {
  fn new(src: String) -> Cursor {
    ret Cursor { src: src, pos: 0 }
  }

  fn at_end(self: &Self) -> bool {
    ret self.pos >= self.src.len()
  }

  fn bump(mut self: &mut Self) -> u8 {
    b := self.src.byte(self.pos)
    self.pos = self.pos + 1
    ret b
  }
}

fn count_digits(src: str) -> i32 {
  mut c := Cursor::new(String::from(src))
  mut n := 0
  while !c.at_end() {
    b := c.bump()
    if b >= 0x30u8 && b <= 0x39u8 {
      n = n + 1
    }
  }
  ret n
}

fn main() -> i32 {
  if count_digits("a1b2") != 2 {
    ret 1
  }
  ret 0
}
