// `fold` carries a state across the elements from the left.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  v.push(3)
  total := v.fold::<i32>(0, (acc: i32, x: &i32) -> acc + *x)
  if total != 6 { ret 1 }
  count := v.fold::<usize>(0, (acc: usize, x: &i32) -> acc + 1)
  if count != 3 { ret 2 }
  mut e := Vec::<i32>::new()
  z := e.fold::<i32>(7, (acc: i32, x: &i32) -> acc + *x)
  if z != 7 { ret 3 }
  ret 0
}
