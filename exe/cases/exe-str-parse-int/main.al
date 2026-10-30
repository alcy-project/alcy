// A decimal parse answers `None` on a non-number or an overflow.
fn main() -> i32 {
  if i64::parse("0").or_panic_with_message("expected a value") != 0 {
    ret 1
  }
  if i64::parse("42").or_panic_with_message("expected a value") != 42 {
    ret 2
  }
  if i64::parse("-7").or_panic_with_message("expected a value") != 0i64 - 7i64 {
    ret 3
  }
  if i64::parse("+7").or_panic_with_message("expected a value") != 7 {
    ret 4
  }
  if i64::parse("9223372036854775807").or_panic_with_message("expected a value") != 9223372036854775807 {
    ret 5
  }
  if i64::parse("-9223372036854775808").or_panic_with_message("expected a value") != 0i64 - 9223372036854775807i64 - 1i64 {
    ret 6
  }
  if i64::parse("9223372036854775808").is_some() {
    ret 7
  }
  if i64::parse("-9223372036854775809").is_some() {
    ret 8
  }
  if i64::parse("").is_some() {
    ret 9
  }
  if i64::parse("-").is_some() {
    ret 10
  }
  if i64::parse("12a").is_some() {
    ret 11
  }
  if i64::parse(" 1").is_some() {
    ret 12
  }
  s := format("-123")
  if i64::parse(s.as_str()).or_panic_with_message("expected a value") != 0i64 - 123i64 {
    ret 13
  }
  ret 0
}
