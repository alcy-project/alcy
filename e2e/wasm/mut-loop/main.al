// Mutable locals live in allocas, so this exercises the shadow stack and
// the load/store path, not just registers.
fn main() -> i32 {
  mut sum := 0
  mut i := 0
  while i < 10 {
    sum += i
    i += 1
  }
  ret sum - 45
}
