// A match over a reference scrutinee binds its payload by reference,
// and a wide payload's binding store once wrote the pointer word with
// the pointee's width, which wasm validation refused. The arm reads the
// payload back through the binding and the answer survives.
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
