// `remove` keeps the order; `swap_remove` does not.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  v.push(3)
  if v.remove(0) != 1 {
    ret 1
  }
  if v.len() != 2 {
    ret 2
  }
  if *v.at(0).unwrap() != 2 {
    ret 3
  }
  if *v.at(1).unwrap() != 3 {
    ret 4
  }
  if v.remove(1) != 3 {
    ret 5
  }
  if v.len() != 1 {
    ret 6
  }
  if *v.at(0).unwrap() != 2 {
    ret 7
  }
  ret 0
}
