// `binary_search_by` halves a view the way `Vec` does.
fn cmp5(x: &i32) -> i32 {
  if *x < 5 { ret 0i32 - 1 }
  if *x > 5 { ret 1i32 }
  ret 0i32
}

fn main() -> i32 {
  a := [1, 3, 5, 7]
  s := a.as_slice()
  if s.binary_search_by(cmp5).unwrap() != 2 { ret 1 }
  b := [1, 3]
  if b.as_slice().binary_search_by(cmp5).is_some() { ret 2 }
  mut e := Vec::<i32>::new()
  if e.as_slice().binary_search_by(cmp5).is_some() { ret 3 }
  ret 0
}
