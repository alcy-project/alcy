// The magnitudes that do fit keep compiling: a suffix, an annotation,
// the position's expected type, and the signed minimum spelled as a
// negation.
fn main() -> i32 {
  a := 2147483647
  b := -2147483648
  c: i64 := 4294967296
  d := 4294967296u64
  e: u64 := 18446744073709551615
  f := 4294967296usize
  g := 0
  mut v := Vec::<u64>::new()
  v.push(18446744073709551615)
  ret 0
}
