fn main() -> i32 {
  mut t := 1
  f := (a: &i32) -> a
  r := f(&t)
  t = 2
  ret *r
}
