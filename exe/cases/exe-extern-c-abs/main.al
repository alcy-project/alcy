// The first C FFI slice: a declaration names a libc symbol, and every
// call goes through the gate.
extern "C" {
  fn abs(x: i32) -> i32;
}

fn main() -> i32 {
  a := unsafe { abs(-42) }
  b := unsafe { abs(7) }
  ret (a - 42) + (b - 7)
}
