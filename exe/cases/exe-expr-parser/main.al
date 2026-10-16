// A precedence-climbing expression parser over a token vector.
struct Token {
  kind: i32,
  value: i64,
}

struct Parser {
  tokens: Vec<Token>,
  pos: usize,
}

impl Parser {
  fn at(self: &Self, kind: i32) -> bool {
    if self.pos >= self.tokens.len() {
      ret false
    }
    ret self.tokens.at(self.pos).unwrap().kind == kind
  }

  fn bump(mut self: &mut Self) -> Token {
    t := self.tokens.at(self.pos).unwrap()
    self.pos = self.pos + 1
    ret Token { kind: t.kind, value: t.value }
  }

  fn factor(mut self: &mut Self) -> Option<i64> {
    if self.at(4) {
      _ := self.bump()
      value := self.expr()?
      if !self.at(5) {
        ret Option::None
      }
      _ := self.bump()
      ret Option::Some(value)
    }
    if !self.at(1) {
      ret Option::None
    }
    t := self.bump()
    ret Option::Some(t.value)
  }

  fn term(mut self: &mut Self) -> Option<i64> {
    mut value := self.factor()?
    while self.at(3) {
      _ := self.bump()
      value = value * self.factor()?
    }
    ret Option::Some(value)
  }

  fn expr(mut self: &mut Self) -> Option<i64> {
    mut value := self.term()?
    while self.at(2) {
      _ := self.bump()
      value = value + self.term()?
    }
    ret Option::Some(value)
  }
}

fn token(kind: i32, value: i64) -> Token {
  ret Token { kind: kind, value: value }
}

fn run(tokens: Vec<Token>) -> Option<i64> {
  mut p := Parser { tokens: tokens, pos: 0 }
  ret p.expr()
}

fn main() -> i32 {
  mut sum := Vec::<Token>::new()
  sum.push(token(1, 2))
  sum.push(token(2, 0))
  sum.push(token(1, 3))
  sum.push(token(3, 0))
  sum.push(token(1, 4))
  match run(sum) {
    Option::Some(v) => {
      if v != 14 {
        ret 1
      }
    }
    Option::None => {
      ret 2
    }
  }
  mut grouped := Vec::<Token>::new()
  grouped.push(token(4, 0))
  grouped.push(token(1, 2))
  grouped.push(token(2, 0))
  grouped.push(token(1, 3))
  grouped.push(token(5, 0))
  grouped.push(token(3, 0))
  grouped.push(token(1, 4))
  match run(grouped) {
    Option::Some(v) => {
      if v != 20 {
        ret 3
      }
    }
    Option::None => {
      ret 4
    }
  }
  ret 0
}
