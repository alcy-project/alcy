// A bubble sort over a mutable vector, the in-place pass a compiler
// uses for a small list.
fn sort_in_place(v: &mut Vec<i32>) {
  n := v.len()
  mut i := 0 as usize
  while i < n {
    mut j := 0 as usize
    while j + 1 < n - i {
      if *v.at(j).unwrap() > *v.at(j + 1).unwrap() {
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
  if *v.at(0).unwrap() != 1 {
    ret 1
  }
  if *v.at(1).unwrap() != 2 {
    ret 2
  }
  if *v.at(2).unwrap() != 3 {
    ret 3
  }
  ret 0
}
