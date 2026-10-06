// A call to a declared C function is an operation the gate covers.
extern "C" {
  fn abs(x: i32) -> i32;
}

fn main() -> i32 {
  ret abs(-42)
}
