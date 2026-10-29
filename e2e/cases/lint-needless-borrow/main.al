fn main() -> i32 {
  n := 7
  r := &n
  s := &*r
  ret *s
}
