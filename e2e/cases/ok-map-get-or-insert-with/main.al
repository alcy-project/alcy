// The default is only built on a miss.
fn zero() -> i32 {
  ret 0
}

fn main() -> i32 {
  mut m := Map::<i32>::new()
  p := m.get_or_insert_with("a", zero)
  *p = 7
  q := m.get_or_insert_with("a", zero)
  if *q != 7 {
    ret 1
  }
  if m.len() != 1 {
    ret 2
  }
  ret 0
}
