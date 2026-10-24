// Byte search mirrors `find` and `rfind`.
fn main() -> i32 {
  if "a,b,c".find_byte(0x2cu8).or_panic_with_message("expected a value") != 1 {
    ret 1
  }
  if "a,b,c".rfind_byte(0x2cu8).or_panic_with_message("expected a value") != 3 {
    ret 2
  }
  if "abc".find_byte(0x2cu8).is_some() {
    ret 3
  }
  if "abc".rfind_byte(0x2cu8).is_some() {
    ret 4
  }
  if "".find_byte(0x61u8).is_some() {
    ret 5
  }
  ret 0
}
