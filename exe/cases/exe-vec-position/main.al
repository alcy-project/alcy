// `position` answers the first accepted index.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(10)
  v.push(20)
  v.push(30)
  if v.position((x: &i32) -> *x == 20).unwrap() != 1 { ret 1 }
  if v.position((x: &i32) -> *x == 99).is_some() { ret 2 }
  if v.position((x: &i32) -> *x > 5).unwrap() != 0 { ret 3 }
  if v.position((x: &i32) -> *x > 25).unwrap() != 2 { ret 4 }
  ret 0
}
