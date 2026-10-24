// A bubble sort over a mutable vector, the in-place pass a compiler
// uses for a small list.
fn sort_in_place(v: &mut Vec<i32>) {
  n := v.len()
  mut i := 0 as usize
  while i < n {
    mut j := 0 as usize
    while j + 1 < n - i {
      if *v.at(j).or_panic_with_message("expected a value") > *v.at(j + 1).or_panic_with_message(
        "expected a value"
      ) {
        v.swap(j, j + 1)
      }
      j = j + 1
    }
    i = i + 1
  }
}

fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(3)
  v.push(1)
  v.push(2)
  sort_in_place(&mut v)
  if *v.at(0).or_panic_with_message("expected a value") != 1 {
    ret 1
  }
  if *v.at(1).or_panic_with_message("expected a value") != 2 {
    ret 2
  }
  if *v.at(2).or_panic_with_message("expected a value") != 3 {
    ret 3
  }
  ret 0
}
