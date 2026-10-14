// `find` answers the first accepted element itself.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(10)
  v.push(20)
  v.push(30)
  if *v.find((x: &i32) -> *x == 20).unwrap() != 20 { ret 1 }
  if v.find((x: &i32) -> *x == 99).is_some() { ret 2 }
  if *v.find((x: &i32) -> *x > 5).unwrap() != 10 { ret 3 }
  ret 0
}
