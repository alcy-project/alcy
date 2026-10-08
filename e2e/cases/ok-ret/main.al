// The ways a non-unit function may end: an explicit ret (with or
// without its semicolon), branches that all return, match arms that
// all return, a diverging call, and a block-bodied closure, which
// keeps its tail value (ADR-0059).
fn direct() -> i32 {
  ret 1;
}

fn branches(c: bool) -> i32 {
  if c {
    ret 2
  } else {
    ret 3
  }
}

fn arms(n: i32) -> i32 {
  match n {
    0 => ret 4,
    _ => ret 5,
  }
}

fn diverges() -> i32 {
  panic("x")
}

fn closure() -> i32 {
  f := (x: i32) -> { x + 1 }
  ret f(1)
}

fn main() {
  _ := direct()
  _ := branches(true)
  _ := arms(0)
  _ := closure()
}
