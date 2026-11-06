// A float literal's value is its digits rounded once, to the literal's
// own type. Reading them through `f64` first rounds twice, and the
// second rounding can land on a neighbour of the value they name.

fn upper_neighbour(value: f32) -> bool {
  ret value == 1.00000011920928955078125f32
}

fn main() -> i32 {
  // A hair above the midpoint between 1.0 and 1 + 2^-23 names the
  // upper neighbour. A read that rounds to `f64` first lands on the
  // midpoint itself, which then ties to the even significand and
  // answers 1.0.
  above := 1.0000000596046447753906250000000000000001f32
  println("{:.17f}", above)
  println("{}", upper_neighbour(above))
  // The same digits kept at `f64` name the midpoint, which that type
  // holds exactly.
  above64 := 1.0000000596046447753906250000000000000001f64
  println("{:.17f}", above64)
  // A hair below the midpoint names 1.0 at either width.
  below := 1.000000059604644775390624999999999999999f32
  println("{}", below == 1.0f32)
  // Just under the overflow threshold is the largest finite `f32`. A
  // `f64` window rounds it past the threshold and answers infinity.
  near := 3.4028235677973366e38f32
  println("{:.17f}", near)
  println("{}", near == 3.4028234663852886e38f32)
  ret 0
}
