// `rposition` walks from the end.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(10)
  v.push(20)
  v.push(10)
  if v.rposition((x: &i32) -> *x == 10).unwrap() != 2 { ret 1 }
  if v.rposition((x: &i32) -> *x == 99).is_some() { ret 2 }
  if v.rposition((x: &i32) -> *x > 5).unwrap() != 2 { ret 3 }
  mut e := Vec::<i32>::new()
  if e.rposition((x: &i32) -> true).is_some() { ret 4 }
  ret 0
}
