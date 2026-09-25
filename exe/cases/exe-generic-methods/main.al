fn same(a: str, b: str) -> bool {
  ret str_len(a) == str_len(b) && str_byte(a, 0) == str_byte(b, 0)
}

fn main() -> i32 {
  mut bad := 0
  a: Box<i32> := Box::Filled(41i32)
  e: Box<i32> := Box::Empty
  if a.or(0i32) != 41 {
    bad = 1
  }
  if e.or(7i32) != 7 {
    bad = 2
  }
  if a.is_empty() {
    bad = 3
  }
  if !e.is_empty() {
    bad = 4
  }
  if a.rec(3, 0i32) != 41 {
    bad = 5
  }
  s: Box<str> := Box::Filled("hi")
  t: Box<str> := Box::Empty
  if !same(s.or("x"), "hi") {
    bad = 6
  }
  if !same(t.or("y"), "y") {
    bad = 7
  }
  n: Box<Box<i32>> := Box::Filled(a)
  if n.or(e).or(0i32) != 41 {
    bad = 8
  }
  b: Box<bool> := Box::Filled(true)
  if b.or(false) != true {
    bad = 9
  }
  ret bad
}

enum Box<T> {
  Filled(T),
  Empty,
}

impl<T> Box<T> {
  fn or(self: Self, default: T) -> T {
    ret match self {
      Box::Filled(v) => v,
      Box::Empty => default,
    }
  }
  fn is_empty(self: Self) -> bool {
    ret match self {
      Box::Filled(_) => false,
      Box::Empty => true,
    }
  }
  fn rec(self: Self, n: i32, default: T) -> T {
    if n <= 0 {
      ret self.or(default)
    }
    ret self.rec(n - 1, default)
  }
}
