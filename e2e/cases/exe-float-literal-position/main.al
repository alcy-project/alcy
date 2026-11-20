// An unsuffixed float literal takes the type its position names, and an
// arithmetic expression passes the type it is read at to both of its
// operands, so `1.5 * 2.0` is `f32` arithmetic wherever an `f32` is
// wanted - beside a suffixed literal too.

fn take(value: f32) -> f32 {
  ret value
}

fn doubled() -> f32 {
  ret 1.5 * 2.0
}

fn main() -> i32 {
  annotated: f32 := 1.5 * 2.0
  if annotated != 3.0f32 {
    ret 1
  }
  beside_one: f32 := 1.5f32 * 2.0
  if beside_one != 3.0f32 {
    ret 2
  }
  beside_other: f32 := 2.0 * 1.5f32
  if beside_other != 3.0f32 {
    ret 3
  }
  if take(1.5 * 2.0) != 3.0f32 {
    ret 4
  }
  if doubled() != 3.0f32 {
    ret 5
  }
  // Two `f32` literals overflow to infinity where `f64` would not, so
  // the product says which width the operands took.
  huge: f32 := 1e38 * 1e2
  if !(huge > 1e38f32) {
    ret 6
  }
  println("{} {} {} {} {}", annotated, beside_one, beside_other, huge > 1e38f32, take(0.5 * 4.0))
  ret 0
}
