// Slice search mirrors `Vec`.
fn main() -> i32 {
  a := [1i32, 2i32, 3i32]
  s := a.as_slice()
  if s.position((x: &i32) -> *x == 2).unwrap() != 1 {
    ret 1
  }
  if s.position((x: &i32) -> *x == 9).is_some() {
    ret 2
  }
  if *s.find((x: &i32) -> *x == 3).unwrap() != 3 {
    ret 3
  }
  if s.find((x: &i32) -> *x == 9).is_some() {
    ret 4
  }
  v := Vec::<i32>::new()
  if v.as_slice().position((x: &i32) -> true).is_some() {
    ret 5
  }
  ret 0
}
