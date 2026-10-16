// Mutable slice ends write through the reference.
fn main() -> i32 {
  mut a := [1i32, 2i32, 3i32]
  s := a.as_mut_slice()
  mut f := s.first_mut().unwrap()
  *f = 10
  mut l := s.last_mut().unwrap()
  *l = 30
  if a[0] != 10 {
    ret 1
  }
  if a[2] != 30 {
    ret 2
  }
  mut v := Vec::<i32>::new()
  if v.as_mut_slice().first_mut().is_some() {
    ret 3
  }
  if v.as_mut_slice().last_mut().is_some() {
    ret 4
  }
  ret 0
}
