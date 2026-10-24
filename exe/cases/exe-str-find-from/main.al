// `find_from` scans from an offset, the shape a token walker takes.
fn main() -> i32 {
  s := "abcabc"
  if s.find_from(0, "abc").or_panic_with_message("expected a value") != 0 {
    ret 1
  }
  if s.find_from(1, "abc").or_panic_with_message("expected a value") != 3 {
    ret 2
  }
  if s.find_from(4, "abc").is_some() {
    ret 3
  }
  if s.find_from(1, "").or_panic_with_message("expected a value") != 1 {
    ret 4
  }
  if s.find_from(9, "a").is_some() {
    ret 5
  }
  if s.find_from(7, "").is_some() {
    ret 6
  }
  ret 0
}
