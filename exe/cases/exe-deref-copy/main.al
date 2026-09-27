fn main() -> i32 {
  mut n := 41
  r := &mut n
  x := *r
  _ := r
  n = 5
  _ := x
  ret n - 5
}
