// `Result<T, E>` equality compares shapes and the matching payload:
// `Ok` with `Ok` through `T`, `Err` with `Err` through `E`, and mixed
// shapes are unequal (ADR-0053).
fn main() -> i32 {
  a: Result<i32, i32> := Result::Ok(1)
  b: Result<i32, i32> := Result::Ok(1)
  c: Result<i32, i32> := Result::Ok(2)
  d: Result<i32, i32> := Result::Err(1)
  e: Result<i32, i32> := Result::Err(1)
  f: Result<i32, i32> := Result::Err(2)

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
  if d == f {
    ret 6
  }
  if !(a != c) {
    ret 7
  }
  if !(d != f) {
    ret 8
  }
  ret 0
}
