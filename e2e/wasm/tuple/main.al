fn pair(n: i32) -> (i32, i32) {
  t := (n, n + 1)
  ret t
}
fn main() -> i32 {
  t := pair(3)
  a := t.0
  b := t.1
  ret a + b - 7
}
