// Floats format as decimal magnitudes: up to six fractional digits,
// rounded half-up and trimmed of trailing zeros, and a mantissa with
// an exponent once the magnitude leaves [1e-4, 1e16). `{:.Nf}` (and
// its `{:.N}` sugar) fixes the fractional digits. The infinity and
// NaN spellings ride the default form.
fn main() -> i32 {
  println("{}", 3.5)
  println("{}", 3.0)
  println("{}", -0.125)
  println("{}", 0.1 + 0.2)
  println("{}", 123456.789)
  println("{}", 1e20)
  println("{}", 1.5e-5)
  println("{}", 0.0001)
  println("{:.2f}", 3.14159)
  println("{:.2}", 3.14159)
  println("{:.0f}", 3.7)
  println("{:.2f}", 0.125)
  println("{}", 1.0f32)
  println("{}", 3.5f32)
  x := 0.0
  println("{}", 1.0 / x)
  println("{}", -1.0 / x)
  println("{}", x / x)
  // A narrowed buffer keeps the total honest for the conversion too.
  mut buf := [0u8; 3]
  out := write("{:.2f}", &mut buf, 3.14159)
  if out.written != 3 {
    ret 1
  }
  if out.total != 4 {
    ret 2
  }
  if buf[0] != 51u8 {
    ret 3
  }
  ret 0
}
