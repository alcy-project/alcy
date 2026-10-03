fn apply(f: (i32) -> i32, x: i32) -> i32 {
  ret f(x) + 1
}

fn inc(x: i32) -> i32 {
  ret x + 1
}

fn main() -> i32 {
  g := (a: i32) -> a + 1
  ret apply(g, 1) + apply(inc, 10) + apply((n) -> n * 2, 3) + ((v: i32) -> v)(4)
}
