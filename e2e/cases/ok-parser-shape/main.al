// A token list and a left-to-right sum, the shape a front end uses.
enum Token {
  Num(i64),
  Plus,
  Minus,
}

fn tokenize(src: str) -> Vec<Token> {
  mut out := Vec::<Token>::new()
  mut i := 0 as usize
  while i < str_len(src) {
    b := src.byte(i)
    if b >= 0x30u8 && b <= 0x39u8 {
      mut value := 0i64
      while i < str_len(src) {
        c := src.byte(i)
        if c < 0x30u8 || c > 0x39u8 {
          break
        }
        value = value * 10i64 + ((c - 0x30u8) as i64)
        i = i + 1
      }
      out.push(Token::Num(value))
      continue
    }
    if b == 0x2bu8 {
      out.push(Token::Plus)
    }
    if b == 0x2du8 {
      out.push(Token::Minus)
    }
    i = i + 1
  }
  ret out
}

fn evaluate(tokens: &Vec<Token>) -> i64 {
  mut sign := 1i64
  mut total := 0i64
  mut i := 0 as usize
  while i < tokens.len() {
    match tokens.at(i).unwrap() {
      Token::Num(value) => {
        total = total + sign * *value
      }
      Token::Plus => {
        sign = 1i64
      }
      Token::Minus => {
        sign = 0i64 - 1i64
      }
    }
    i = i + 1
  }
  ret total
}

fn main() -> i32 {
  tokens := tokenize("10 + 20 - 5")
  if evaluate(&tokens) != 25i64 {
    ret 1
  }
  ret 0
}
