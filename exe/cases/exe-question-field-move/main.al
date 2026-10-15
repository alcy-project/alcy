// `?` on a field takes its payload out of the field, so the field's
// place is moved and the owner's drop skips it.
struct S {
  b: Option<Vec<i32>>,
}

fn take(s: S) -> Option<Vec<i32>> {
  inner := s.b?
  ret Option::Some(inner)
}

fn main() -> i32 {
  mut items := Vec::<i32>::new()
  items.push(7)
  s := S { b: Option::Some(items) }
  match take(s) {
    Option::Some(v) => {
      if v.len() != 1 { ret 1 }
      if *v.at(0).unwrap() != 7 { ret 2 }
    }
    Option::None => {
      ret 3
    }
  }
  ret 0
}
