fn apply(f: (i32) -> i32, x: i32) -> i32 {
  ret f(x) + 1
}

fn twice(f: (i32) -> i32, x: i32) -> i32 {
  ret f(f(x))
}

fn main() -> i32 {
  g := (a: i32) -> a * 2
  if apply(g, 20) != 41 { ret 1 }
  if twice(g, 10) != 40 { ret 2 }
  if ((v: i32) -> v - 1)(50) != 49 { ret 3 }
  ret 0
}
