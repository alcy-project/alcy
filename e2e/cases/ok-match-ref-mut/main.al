// A `&mut` scrutinee binds its payloads as exclusive references.
enum Token {
  Num(i64),
  Plus,
}

fn bump(t: &mut Token) -> i64 {
  match t {
    Token::Num(v) => {
      *v = *v + 1i64
      ret *v
    }
    Token::Plus => {
      ret 0i64
    }
  }
}

fn main() -> i32 {
  mut t := Token::Num(7i64)
  if bump(&mut t) != 8i64 { ret 1 }
  ret 0
}
