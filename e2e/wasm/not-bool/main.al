fn main() -> i32 {
  x := true
  if !x {
    ret 1
  }
  y := false
  if !y {
    ret 0
  }
  ret 2
}
