// `split_at` answers the two slices around a boundary.
fn main() -> i32 {
  a := [1i32, 2i32, 3i32]
  s := a.as_slice()
  (x, y) := s.split_at(1)
  if x.len() != 1 {
    ret 1
  }
  if *x.at(0).or_panic_with_message("expected a value") != 1 {
    ret 2
  }
  if y.len() != 2 {
    ret 3
  }
  if *y.at(0).or_panic_with_message("expected a value") != 2 {
    ret 4
  }
  (p, q) := s.split_at(0)
  if p.len() != 0 {
    ret 5
  }
  if q.len() != 3 {
    ret 6
  }
  (m, n) := s.split_at(3)
  if m.len() != 3 {
    ret 7
  }
  if n.len() != 0 {
    ret 8
  }
  ret 0
}
