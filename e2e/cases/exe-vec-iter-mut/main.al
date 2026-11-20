// `for mut x in v.iter_mut()` hands out exclusive references; writing
// through one needs a mutable binding to name it.

fn main() -> i32 {
  mut v: Vec<i32> := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  for mut x in v.iter_mut() {
    *x = *x + 10
  }
  if *v.at(0).or_panic_with_message("expected a value") != 11 {
    ret 1
  }
  if *v.at(1).or_panic_with_message("expected a value") != 12 {
    ret 2
  }
  ret 0
}
