// `dedup_by` drops consecutive repeats, keeping the order.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(1)
  v.push(2)
  v.push(2)
  v.push(2)
  v.push(3)
  v.push(1)
  v.dedup_by((a: &i32, b: &i32) -> *a == *b)
  if v.len() != 4 { ret 1 }
  if *v.at(0).unwrap() != 1 { ret 2 }
  if *v.at(1).unwrap() != 2 { ret 3 }
  if *v.at(2).unwrap() != 3 { ret 4 }
  if *v.at(3).unwrap() != 1 { ret 5 }
  mut e := Vec::<i32>::new()
  e.dedup_by((a: &i32, b: &i32) -> true)
  if e.len() != 0 { ret 6 }
  ret 0
}
