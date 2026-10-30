// A `{:.N}` precision reads the same as `{:.Nf}`, but the explicit
// conversion is the one the project prefers; the difference is a
// warning, not an error.
fn main() -> i32 {
  a := format("{:.2f}", 3.14159)
  b := format("{:.2}", 3.14159)
  if a.len_bytes() != b.len_bytes() {
    ret 1
  }
  if format("{}", 7i32).len_bytes() != 1 {
    ret 2
  }
  ret 0
}
