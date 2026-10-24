// An unsigned parse takes `+` and rejects `-` and overflow.
fn main() -> i32 {
  if "0".parse_u64().or_panic_with_message("expected a value") != 0u64 {
    ret 1
  }
  if "42".parse_u64().or_panic_with_message("expected a value") != 42u64 {
    ret 2
  }
  if "+42".parse_u64().or_panic_with_message("expected a value") != 42u64 {
    ret 3
  }
  if "18446744073709551615".parse_u64().or_panic_with_message("expected a value") != 18446744073709551615u64 {
    ret 4
  }
  if "18446744073709551616".parse_u64().is_some() {
    ret 5
  }
  if "-1".parse_u64().is_some() {
    ret 6
  }
  if "".parse_u64().is_some() {
    ret 7
  }
  if "abc".parse_u64().is_some() {
    ret 8
  }
  s := format("99")
  if s.parse_u64().or_panic_with_message("expected a value") != 99u64 {
    ret 9
  }
  ret 0
}
