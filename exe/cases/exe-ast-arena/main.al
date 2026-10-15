// An index-based AST: nodes live in one vector and refer to each other
// by position, the way a self-hosted parser avoids recursive types.
struct Token {
  kind: i32,
  value: i64,
}

enum Expr {
  Num(i64),
  Add(usize, usize),
  Mul(usize, usize),
}

struct Parser {
  tokens: Vec<Token>,
  pos: usize,
  nodes: Vec<Expr>,
}

impl Parser {
  fn at(self: &Self, kind: i32) -> bool {
    if self.pos >= self.tokens.len() {
      ret false
    }
    ret self.tokens.at(self.pos).unwrap().kind == kind
  }

  fn bump(mut self: &mut Self) -> i64 {
    v := self.tokens.at(self.pos).unwrap().value
    self.pos = self.pos + 1
    ret v
  }

  fn node(mut self: &mut Self, e: Expr) -> usize {
    idx := self.nodes.len()
    self.nodes.push(e)
    ret idx
  }

  fn factor(mut self: &mut Self) -> Option<usize> {
    if self.at(4) {
      _ := self.bump()
      inner := self.expr()?
      if !self.at(5) {
        ret Option::None
      }
      _ := self.bump()
      ret Option::Some(inner)
    }
    if !self.at(1) {
      ret Option::None
    }
    v := self.bump()
    ret Option::Some(self.node(Expr::Num(v)))
  }

  fn term(mut self: &mut Self) -> Option<usize> {
    mut left := self.factor()?
    while self.at(3) {
      _ := self.bump()
      right := self.factor()?
      left = self.node(Expr::Mul(left, right))
    }
    ret Option::Some(left)
  }

  fn expr(mut self: &mut Self) -> Option<usize> {
    mut left := self.term()?
    while self.at(2) {
      _ := self.bump()
      right := self.term()?
      left = self.node(Expr::Add(left, right))
    }
    ret Option::Some(left)
  }
}

fn eval(nodes: &Vec<Expr>, idx: usize) -> i64 {
  match nodes.at(idx).unwrap() {
    Expr::Num(v) => {
      ret *v
    }
    Expr::Add(l, r) => {
      ret eval(nodes, *l) + eval(nodes, *r)
    }
    Expr::Mul(l, r) => {
      ret eval(nodes, *l) * eval(nodes, *r)
    }
  }
}

fn run(tokens: Vec<Token>) -> Option<i64> {
  mut p := Parser { tokens: tokens, pos: 0, nodes: Vec::<Expr>::new() }
  match p.expr() {
    Option::Some(root) => {
      ret Option::Some(eval(&p.nodes, root))
    }
    Option::None => {
      ret Option::None
    }
  }
}

fn token(kind: i32, value: i64) -> Token {
  ret Token { kind: kind, value: value }
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
      if v != 14 { ret 1 }
    }
    Option::None => {
      ret 2
    }
  }
  ret 0
}
