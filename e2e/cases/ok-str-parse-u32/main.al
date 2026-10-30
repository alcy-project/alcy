// A decimal parse that must fit `u32`.
fn main() -> i32 {
  if u32::parse("42").or_panic_with_message("expected a value") != 42u32 {
    ret 1
  }
  if u32::parse("+42").or_panic_with_message("expected a value") != 42u32 {
    ret 2
  }
  if u32::parse("4294967295").or_panic_with_message("expected a value") != 4294967295u32 {
    ret 3
  }
  if u32::parse("4294967296").is_some() {
    ret 4
  }
  if u32::parse("-1").is_some() {
    ret 5
  }
  if u32::parse("abc").is_some() {
    ret 6
  }
  s := format("99")
  if u32::parse(s.as_str()).or_panic_with_message("expected a value") != 99u32 {
    ret 7
  }
  ret 0
}
