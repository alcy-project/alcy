// `?` propagates a custom error out of a `Result` method.
enum ParseError {
  Unexpected(u8),
  End,
}

struct Parser {
  src: String,
  pos: usize,
}

impl Parser {
  fn byte(mut self: &mut Self) -> Result<u8, ParseError> {
    if self.pos >= self.src.len_bytes() {
      ret Result::Err(ParseError::End)
    }
    b := self.src.byte(self.pos)
    self.pos = self.pos + 1
    ret Result::Ok(b)
  }

  fn digit(mut self: &mut Self) -> Result<u8, ParseError> {
    b := self.byte()?
    if b < 0x30u8 || b > 0x39u8 {
      ret Result::Err(ParseError::Unexpected(b))
    }
    ret Result::Ok(b)
  }
}

fn main() -> i32 {
  mut p := Parser { src: String::from("7"), pos: 0 }
  match p.digit() {
    Result::Ok(v) => {
      if v != 0x37u8 {
        ret 1
      }
    }
    Result::Err(_) => {
      ret 2
    }
  }
  mut e := Parser { src: String::from(""), pos: 0 }
  match e.digit() {
    Result::Ok(_) => {
      ret 3
    }
    Result::Err(why) => {
      match why {
        ParseError::End => {}
        ParseError::Unexpected(_) => {
          ret 4
        }
      }
    }
  }
  ret 0
}
