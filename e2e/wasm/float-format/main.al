// A float crosses the format path as an `f64`: the digit walk reads,
// writes, and multiplies floats through field and element addresses,
// which are words whatever the register beside them names.
fn main() {
  println("{}", 1.5)
  println("{:.2f}", 2.0 / 3.0)
  println("{}", 0.125)
}
