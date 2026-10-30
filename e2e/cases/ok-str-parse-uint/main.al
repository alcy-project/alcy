// An unsigned parse takes `+` and rejects `-` and overflow.
fn main() -> i32 {
  if u64::parse("0").or_panic_with_message("expected a value") != 0u64 {
    ret 1
  }
  if u64::parse("42").or_panic_with_message("expected a value") != 42u64 {
    ret 2
  }
  if u64::parse("+42").or_panic_with_message("expected a value") != 42u64 {
    ret 3
  }
  if u64::parse("18446744073709551615").or_panic_with_message("expected a value") != 18446744073709551615u64 {
    ret 4
  }
  if u64::parse("18446744073709551616").is_some() {
    ret 5
  }
  if u64::parse("-1").is_some() {
    ret 6
  }
  if u64::parse("").is_some() {
    ret 7
  }
  if u64::parse("abc").is_some() {
    ret 8
  }
  s := format("99")
  if u64::parse(s.as_str()).or_panic_with_message("expected a value") != 99u64 {
    ret 9
  }
  ret 0
}
