// Or-patterns that bind the same name in every alternative.
enum Tok {
  Plus(i32),
  Minus(i32),
  End,
}

fn weight(t: Tok) -> i32 {
  match t {
    Tok::Plus(n) | Tok::Minus(n) => {
      ret n
    }
    Tok::End => {
      ret 0
    }
  }
}

fn main() -> i32 {
  if weight(Tok::Plus(3)) != 3 { ret 1 }
  if weight(Tok::Minus(4)) != 4 { ret 2 }
  if weight(Tok::End) != 0 { ret 3 }
  ret 0
}
