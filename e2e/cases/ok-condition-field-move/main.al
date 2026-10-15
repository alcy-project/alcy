// A pattern condition takes a non-Copy payload out of a field: the
// field's place is moved, so the owner's drop skips it.
struct S {
  b: Option<Vec<i32>>,
}

fn main() -> i32 {
  mut items := Vec::<i32>::new()
  items.push(7)
  s := S { b: Option::Some(items) }
  if Option::Some(inner) := s.b {
    if inner.len() != 1 { ret 1 }
    if *inner.at(0).unwrap() != 7 { ret 2 }
  } else {
    ret 3
  }
  ret 0
}
