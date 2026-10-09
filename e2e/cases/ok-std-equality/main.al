// Equality over the standard generic shapes: `Option<T>`,
// `Result<T, E>`, and `Set` resolve `==` through `PartialEq`.
fn main() -> i32 {
  a := Option::Some(1)
  b := Option::Some(1)
  c: Option<i32> := Option::None
  if !(a == b) {
    ret 1
  }
  if a == c {
    ret 2
  }

  ok: Result<i32, i32> := Result::Ok(1)
  ok_too: Result<i32, i32> := Result::Ok(1)
  err: Result<i32, i32> := Result::Err(1)
  if !(ok == ok_too) {
    ret 3
  }
  if ok == err {
    ret 4
  }

  mut left := Set::new()
  _ := left.insert("x")
  mut right := Set::new()
  _ := right.insert("x")
  if !(left == right) {
    ret 5
  }
  ret 0
}
