// A string hands out its bytes exclusively, so a byte can be edited.
// The view is scoped, so the loan ends before the string is read.
fn main() -> i32 {
  mut s := format("abc")
  {
    b := s.as_mut_slice()
    if b.len() != 3 {
      ret 1
    }
    b[0] = 0x41u8
    b[2] = 0x5au8
  }
  if s.as_str() != "AbZ" {
    ret 2
  }
  ret 0
}
