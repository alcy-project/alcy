// `swap_remove` fills the hole with the last element.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  v.push(3)
  if v.swap_remove(0) != 1 { ret 1 }
  if v.len() != 2 { ret 2 }
  if *v.at(0).unwrap() != 3 { ret 3 }
  if *v.at(1).unwrap() != 2 { ret 4 }
  mut w := Vec::<i32>::new()
  w.push(7)
  if w.swap_remove(0) != 7 { ret 5 }
  if w.len() != 0 { ret 6 }
  ret 0
}
