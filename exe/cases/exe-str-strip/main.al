// `strip_prefix`/`strip_suffix` answer a sub-view or `None`.
fn main() -> i32 {
  if "hello".strip_prefix("he").or_panic_with_message("expected a value") != "llo" {
    ret 1
  }
  if "hello".strip_prefix("xx").is_some() {
    ret 2
  }
  if "hello".strip_suffix("lo").or_panic_with_message("expected a value") != "hel" {
    ret 3
  }
  if "hello".strip_suffix("xx").is_some() {
    ret 4
  }
  if "hello".strip_prefix("").or_panic_with_message("expected a value") != "hello" {
    ret 5
  }
  if "hello".strip_suffix("").or_panic_with_message("expected a value") != "hello" {
    ret 6
  }
  if "hello".strip_prefix("hello").or_panic_with_message("expected a value") != "" {
    ret 7
  }
  s := format("--flag")
  if s.strip_prefix("--").or_panic_with_message("expected a value") != "flag" {
    ret 8
  }
  ret 0
}
