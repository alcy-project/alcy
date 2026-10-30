// A type argument infers from a parameter whose declared type is that
// parameter itself.
fn id<T>(x: T) -> T {
  ret x
}

fn main() -> i32 {
  if id(5) != 5 {
    ret 1
  }
  if id("x") != "x" {
    ret 2
  }
  n := id(7)
  if n != 7 {
    ret 3
  }
  ret 0
}
