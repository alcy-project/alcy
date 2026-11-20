// A byte insert past the length answers `None`, and the string keeps
// the bytes it held.
fn main() -> i32 {
  mut s := format("ab")
  match s.insert_str(5, "X") {
    Option::Some(_) => {
      ret 1
    }
    Option::None => {}
  }
  if s.as_str() != "ab" {
    ret 2
  }
  ret 0
}
