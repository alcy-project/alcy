// A decimal parse answers `None` on a non-number or an overflow.
fn main() -> i32 {
  if "0".parse_i64().unwrap() != 0 { ret 1 }
  if "42".parse_i64().unwrap() != 42 { ret 2 }
  if "-7".parse_i64().unwrap() != 0i64 - 7i64 { ret 3 }
  if "+7".parse_i64().unwrap() != 7 { ret 4 }
  if "9223372036854775807".parse_i64().unwrap() != 9223372036854775807 { ret 5 }
  if "-9223372036854775808".parse_i64().unwrap() != 0i64 - 9223372036854775807i64 - 1i64 { ret 6 }
  if "9223372036854775808".parse_i64().is_some() { ret 7 }
  if "-9223372036854775809".parse_i64().is_some() { ret 8 }
  if "".parse_i64().is_some() { ret 9 }
  if "-".parse_i64().is_some() { ret 10 }
  if "12a".parse_i64().is_some() { ret 11 }
  if " 1".parse_i64().is_some() { ret 12 }
  s := format("-123")
  if s.parse_i64().unwrap() != 0i64 - 123i64 { ret 13 }
  ret 0
}
