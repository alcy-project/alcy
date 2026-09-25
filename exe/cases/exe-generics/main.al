fn unwrap_or(x: Box<i32>, default: i32) -> i32 {
  ret match x {
    Box::Filled(v) => v,
    Box::Empty => default,
  }
}

fn main() -> i32 {
  mut bad := 0
  a: Box<i32> := Box::Filled(41i32)
  if unwrap_or(a, 0) != 41 {
    bad = 1
  }
  e: Box<i32> := Box::Empty
  if unwrap_or(e, 7) != 7 {
    bad = 2
  }
  p: Pair<i32, str> := Pair::Both(1i32, "hi")
  n := match p {
    Pair::Both(x, s) => x + str_len(s) as i32,
    Pair::Neither => -1,
  }
  if n != 3 {
    bad = 3
  }
  ret bad
}

enum Box<T> { Filled(T), Empty }

enum Pair<A, B> { Both(A, B), Neither }
