// A decimal parse that must fit `i32`.
fn main() -> i32 {
  if i32::parse("42").or_panic_with_message("expected a value") != 42 {
    ret 1
  }
  if i32::parse("-7").or_panic_with_message("expected a value") != 0 - 7 {
    ret 2
  }
  if i32::parse("2147483647").or_panic_with_message("expected a value") != 2147483647 {
    ret 3
  }
  if i32::parse("2147483648").is_some() {
    ret 4
  }
  if i32::parse("-2147483648").or_panic_with_message("expected a value") != 0 - 2147483647 - 1 {
    ret 5
  }
  if i32::parse("-2147483649").is_some() {
    ret 6
  }
  if i32::parse("abc").is_some() {
    ret 7
  }
  s := format("99")
  if i32::parse(s.as_str()).or_panic_with_message("expected a value") != 99 {
    ret 8
  }
  ret 0
}
