// A decimal parse that must fit `u32`.
fn main() -> i32 {
  if "42".parse_u32().unwrap() != 42u32 {
    ret 1
  }
  if "+42".parse_u32().unwrap() != 42u32 {
    ret 2
  }
  if "4294967295".parse_u32().unwrap() != 4294967295u32 {
    ret 3
  }
  if "4294967296".parse_u32().is_some() {
    ret 4
  }
  if "-1".parse_u32().is_some() {
    ret 5
  }
  if "abc".parse_u32().is_some() {
    ret 6
  }
  s := format("99")
  if s.parse_u32().unwrap() != 99u32 {
    ret 7
  }
  ret 0
}
