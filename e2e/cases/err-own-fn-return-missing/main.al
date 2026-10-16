// A body that never names the signature has no witness to give.
fn bad() -> fn (i32) -> i32 {
  while true {}
}

fn main() -> i32 {
  f := bad()
  ret f(1)
}
