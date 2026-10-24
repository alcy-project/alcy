// Slice ends answer `Option<&T>`, so an empty one is not special.
fn main() -> i32 {
  a := [1i32, 2i32, 3i32]
  s := a.as_slice()
  if *s.first().or_panic_with_message("expected a value") != 1 {
    ret 1
  }
  if *s.last().or_panic_with_message("expected a value") != 3 {
    ret 2
  }
  v := Vec::<i32>::new()
  if v.as_slice().first().is_some() {
    ret 3
  }
  if v.as_slice().last().is_some() {
    ret 4
  }
  ret 0
}
