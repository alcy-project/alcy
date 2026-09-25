// Generic free functions: argument-driven inference, explicit
// turbofish arguments, a parameter shared across arguments, recursion
// inside a generic body, and generic struct arguments.
struct Box<T> { item: T }

fn identity<T>(x: T) -> T {
  ret x
}

fn unwrap_or<T>(b: Box<T>, fallback: T) -> T {
  ret b.item
}

fn pair_up<T>(a: T, b: T) -> Box<T> {
  ret Box { item: a }
}

fn recurse<T>(x: T, n: i32) -> T {
  if n <= 0 {
    ret x
  }
  ret recurse(x, n - 1)
}

fn same(a: str, b: str) -> bool {
  ret str_len(a) == str_len(b) && str_byte(a, 0) == str_byte(b, 0)
}

fn main() -> i32 {
  mut bad := 0
  if identity(41i32) != 41 {
    bad = 1
  }
  if !identity(true) {
    bad = 2
  }
  if str_len(identity("hi")) != 2 {
    bad = 3
  }
  if identity::<i32>(7i32) != 7 {
    bad = 4
  }
  p := pair_up(20i32, 30i32)
  if p.item != 20 {
    bad = 5
  }
  bx: Box<i32> := Box { item: 5i32 }
  if unwrap_or(bx, 0i32) != 5 {
    bad = 6
  }
  nb: Box<Box<i32>> := Box { item: bx }
  if unwrap_or(nb, bx).item != 5 {
    bad = 7
  }
  if recurse(9i32, 3) != 9 {
    bad = 8
  }
  if !same(recurse("abc", 2), "abc") {
    bad = 9
  }
  ret bad
}
