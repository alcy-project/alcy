// A decimal parse that must fit `i32`.
fn main() -> i32 {
  if "42".parse_i32().unwrap() != 42 {
    ret 1
  }
  if "-7".parse_i32().unwrap() != 0 - 7 {
    ret 2
  }
  if "2147483647".parse_i32().unwrap() != 2147483647 {
    ret 3
  }
  if "2147483648".parse_i32().is_some() {
    ret 4
  }
  if "-2147483648".parse_i32().unwrap() != 0 - 2147483647 - 1 {
    ret 5
  }
  if "-2147483649".parse_i32().is_some() {
    ret 6
  }
  if "abc".parse_i32().is_some() {
    ret 7
  }
  s := format("99")
  if s.parse_i32().unwrap() != 99 {
    ret 8
  }
  ret 0
}
