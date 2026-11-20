// `for mut x in s.iter_mut()` writes through a slice or an array.

fn main() -> i32 {
  mut v: Vec<i32> := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  s := v.as_mut_slice()
  for mut x in s.iter_mut() {
    *x = *x + 10
  }
  if *v.at(0).or_panic_with_message("expected a value") != 11 {
    ret 1
  }
  if *v.at(1).or_panic_with_message("expected a value") != 12 {
    ret 2
  }

  mut a := [1i32, 2i32, 3i32]
  for mut x in a.iter_mut() {
    *x = *x * 2
  }
  if a[0] != 2 {
    ret 3
  }
  if a[2] != 6 {
    ret 4
  }
  ret 0
}
