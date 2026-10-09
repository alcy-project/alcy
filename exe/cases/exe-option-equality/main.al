// `Option<T>` equality compares shapes and payloads: `Some` with
// `Some` through `PartialEq`, `None` with `None`, and mixed shapes are
// unequal (ADR-0053).
fn main() -> i32 {
  a := Option::Some(1)
  b := Option::Some(1)
  c := Option::Some(2)
  d: Option<i32> := Option::None
  e: Option<i32> := Option::None

  if !(a == b) {
    ret 1
  }
  if a == c {
    ret 2
  }
  if a == d {
    ret 3
  }
  if d == a {
    ret 4
  }
  if !(d == e) {
    ret 5
  }
  if !(a != c) {
    ret 6
  }
  if d != e {
    ret 7
  }
  ret 0
}
