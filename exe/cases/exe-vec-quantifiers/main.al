// Quantifiers; the empty vector is vacuously all/never any.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(2)
  v.push(4)
  v.push(6)
  if !v.all((x: &i32) -> *x % 2 == 0) {
    ret 1
  }
  if v.all((x: &i32) -> *x > 3) {
    ret 2
  }
  if !v.any((x: &i32) -> *x == 4) {
    ret 3
  }
  if v.any((x: &i32) -> *x == 5) {
    ret 4
  }
  mut e := Vec::<i32>::new()
  if !e.all((x: &i32) -> false) {
    ret 5
  }
  if e.any((x: &i32) -> true) {
    ret 6
  }
  ret 0
}
