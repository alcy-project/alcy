// `[T]::starts_with` matches a prefix, an empty one included.
fn main() -> i32 {
  a := [1, 2, 3]
  p := [1, 2]
  if !a.as_slice().starts_with(p.as_slice()) {
    ret 1
  }
  q := [2]
  if a.as_slice().starts_with(q.as_slice()) {
    ret 2
  }
  b := [1, 2, 3, 4]
  if a.as_slice().starts_with(b.as_slice()) {
    ret 3
  }
  mut e := Vec::<i32>::new()
  if !a.as_slice().starts_with(e.as_slice()) {
    ret 4
  }
  ret 0
}
