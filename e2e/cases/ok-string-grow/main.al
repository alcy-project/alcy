// A string grows past the old fixed limit by reallocating, through
// pushes and through formatting alike.
fn main() -> i32 {
  mut s := String::new()
  mut i := 0
  while i < 300 {
    s.push(97u8)
    i = i + 1
  }
  if s.len() != 300 {
    ret 1
  }
  big := format("{}{}{}", (s.as_str(), s.as_str(), s.as_str()))
  if str_len(big.as_str()) != 900 {
    ret 2
  }
  ret 0
}
