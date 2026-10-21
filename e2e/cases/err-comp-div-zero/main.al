// A constant expression that divides by zero is refused with its own
// message, the way a checked build refuses it at runtime.
fn main() -> i32 {
  ret comp { 1 / 0 }
}
