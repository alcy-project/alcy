// A format-heavy body for the fmt pilot's baseline: text, integers,
// floats, and the precision forms the conversion lane owns today.
fn main() -> i32 {
  mut total := 0
  mut i := 0
  while i < 20 {
    line := format("{} {:.6} {:.6f} {} {:.17f}", i, 1.0 / 3.0, 0.1, "text", 0.7)
    total += line.len_bytes() as i32
    i += 1
  }
  if total <= 0 {
    ret 1
  }
  ret 0
}
