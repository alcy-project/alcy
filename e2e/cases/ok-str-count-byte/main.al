// `count_byte` tallies one byte.
fn main() -> i32 {
  if "a,b,c".count_byte(0x2cu8) != 2 {
    ret 1
  }
  if "".count_byte(0x2cu8) != 0 {
    ret 2
  }
  if "aaa".count_byte(0x61u8) != 3 {
    ret 3
  }
  s := format("xxy")
  if s.as_str().count_byte(0x78u8) != 2 {
    ret 4
  }
  ret 0
}
