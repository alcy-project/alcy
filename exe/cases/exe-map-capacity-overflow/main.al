// A `with_capacity` whose slot count would overflow is refused rather
// than silently rounded down. The request is larger than three quarters
// of the address space, so no allocation is attempted.
fn main() -> i32 {
  huge := ((0 as usize) - 1) / 4 * 3 + 1
  mut m := Map::<i32>::with_capacity(huge)
  _ := m.insert("x", 1)
  ret 0
}
