fn main() -> i32 {
  const a := true
  const b := 1i32
  const c := 2i32
  const d := 4i32
  if b < c && d < b {
    ret 1
  }
  if !(b < c || d < b) {
    ret 2
  }
  if !(b < c && a) {
    ret 3
  }
  if a != (b < c) {
    ret 4
  }
  if !((b < c) == a) {
    ret 5
  }
  ret 0
}
