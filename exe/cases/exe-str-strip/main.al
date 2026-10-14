// `strip_prefix`/`strip_suffix` answer a sub-view or `None`.
fn main() -> i32 {
  if "hello".strip_prefix("he").unwrap() != "llo" { ret 1 }
  if "hello".strip_prefix("xx").is_some() { ret 2 }
  if "hello".strip_suffix("lo").unwrap() != "hel" { ret 3 }
  if "hello".strip_suffix("xx").is_some() { ret 4 }
  if "hello".strip_prefix("").unwrap() != "hello" { ret 5 }
  if "hello".strip_suffix("").unwrap() != "hello" { ret 6 }
  if "hello".strip_prefix("hello").unwrap() != "" { ret 7 }
  s := format("--flag")
  if s.strip_prefix("--").unwrap() != "flag" { ret 8 }
  ret 0
}
