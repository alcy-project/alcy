// `split_first` and `split_last` answer an end and the rest.
fn main() -> i32 {
  a := [1, 2, 3]
  s := a.as_slice()
  pair := s.split_first().unwrap()
  if *pair.0 != 1 { ret 1 }
  if pair.1.len() != 2 { ret 2 }
  back := s.split_last().unwrap()
  if *back.0 != 3 { ret 3 }
  if back.1.len() != 2 { ret 4 }
  mut v := Vec::<i32>::new()
  if v.as_slice().split_first().is_some() { ret 5 }
  if v.as_slice().split_last().is_some() { ret 6 }
  ret 0
}
