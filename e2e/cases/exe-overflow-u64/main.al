// Unsigned 64-bit addition wraps in hardware, so the checked form has
// to detect the carry itself.
fn main() -> i32 {
  x := 18446744073709551615u64
  y := x + 1u64
  println("{}", y)
  ret 0
}
