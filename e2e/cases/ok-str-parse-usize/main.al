// `usize::parse` reads an unsigned machine word from text.
fn main() -> i32 {
  if usize::parse("42").or_panic_with_message("expected a value") != 42 as usize {
    ret 1
  }
  if usize::parse("x").is_some() {
    ret 2
  }
  if usize::parse("-1").is_some() {
    ret 3
  }
  if usize::parse("").is_some() {
    ret 4
  }
  mut s := format("7")
  if usize::parse(s.as_str()).or_panic_with_message("expected a value") != 7 as usize {
    ret 5
  }
  ret 0
}
