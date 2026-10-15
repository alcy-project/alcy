// A value pattern matches a reference scrutinee directly.
enum Token {
  Num(i64),
  Plus,
}

fn classify(t: &Token) -> i64 {
  match t {
    Token::Num(v) => {
      ret *v
    }
    Token::Plus => {
      ret 0i64
    }
  }
}

fn main() -> i32 {
  t := Token::Num(7i64)
  if classify(&t) != 7i64 { ret 1 }
  p := Token::Plus
  if classify(&p) != 0i64 { ret 2 }
  ret 0
}
