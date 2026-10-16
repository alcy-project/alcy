// The owned string forwards the byte searches a view answers.
fn main() -> i32 {
  s := format("a,b,c")
  if s.find_byte(0x2cu8).unwrap() != 1 {
    ret 1
  }
  if s.rfind_byte(0x2cu8).unwrap() != 3 {
    ret 2
  }
  if s.count_byte(0x2cu8) != 2 {
    ret 3
  }
  t := format("abab")
  if t.count_matches("ab") != 2 {
    ret 4
  }
  ret 0
}
