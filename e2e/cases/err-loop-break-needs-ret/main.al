// A `break` reaches the exit, so the function still needs a `ret`.
fn g() -> i32 {
  loop { break }
}
fn main() -> i32 { ret g() }
