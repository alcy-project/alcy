// A recursive expression tree walked through references, the shape a
// front end holds after parsing.
enum Expr {
  Num(i64),
  Neg(&Expr),
  Add(&Expr, &Expr),
}

fn eval(e: &Expr) -> i64 {
  match e {
    Expr::Num(n) => { ret *n }
    Expr::Neg(inner) => { ret 0i64 - eval(*inner) }
    Expr::Add(a, b) => { ret eval(*a) + eval(*b) }
  }
}

fn main() -> i32 {
  a := Expr::Num(2i64)
  b := Expr::Num(3i64)
  sum := Expr::Add(&a, &b)
  if eval(&sum) != 5i64 { ret 1 }
  neg := Expr::Neg(&sum)
  if eval(&neg) != 0i64 - 5i64 { ret 2 }
  ret 0
}
