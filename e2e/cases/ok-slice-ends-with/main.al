// `[T]::ends_with` matches a suffix, an empty one included.
fn main() -> i32 {
  a := [1, 2, 3]
  p := [2, 3]
  if !a.as_slice().ends_with(p.as_slice()) {
    ret 1
  }
  q := [2]
  if a.as_slice().ends_with(q.as_slice()) {
    ret 2
  }
  b := [1, 2, 3, 4]
  if b.as_slice().ends_with(a.as_slice()) {
    ret 3
  }
  mut e := Vec::<i32>::new()
  if !a.as_slice().ends_with(e.as_slice()) {
    ret 4
  }
  ret 0
}
