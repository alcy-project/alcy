// `find_from` scans from an offset, the shape a token walker takes.
fn main() -> i32 {
  s := "abcabc"
  if s.find_from(0, "abc").unwrap() != 0 {
    ret 1
  }
  if s.find_from(1, "abc").unwrap() != 3 {
    ret 2
  }
  if s.find_from(4, "abc").is_some() {
    ret 3
  }
  if s.find_from(1, "").unwrap() != 1 {
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
