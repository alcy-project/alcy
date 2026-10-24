// `reverse` swaps the ends inward.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  v.push(3)
  v.reverse()
  if *v.at(0).or_panic_with_message("expected a value") != 3 {
    ret 1
  }
  if *v.at(1).or_panic_with_message("expected a value") != 2 {
    ret 2
  }
  if *v.at(2).or_panic_with_message("expected a value") != 1 {
    ret 3
  }
  mut w := Vec::<i32>::new()
  w.push(9)
  w.reverse()
  if *w.at(0).or_panic_with_message("expected a value") != 9 {
    ret 4
  }
  mut e := Vec::<i32>::new()
  e.reverse()
  if e.len() != 0 {
    ret 5
  }
  ret 0
}
