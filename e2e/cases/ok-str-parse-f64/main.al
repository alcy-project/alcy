// A decimal float folds its digits into one numerator and one division.
fn main() -> i32 {
  if f64::parse("3.5").or_panic_with_message("expected a value") != 3.5 {
    ret 1
  }
  if f64::parse("0.5").or_panic_with_message("expected a value") != 0.5 {
    ret 2
  }
  if f64::parse("-2.25").or_panic_with_message("expected a value") != 0.0f64 - 2.25 {
    ret 3
  }
  if f64::parse("3.14").or_panic_with_message("expected a value") != 3.14 {
    ret 4
  }
  if f64::parse("0.1").or_panic_with_message("expected a value") != 0.1 {
    ret 5
  }
  if f64::parse("1").or_panic_with_message("expected a value") != 1.0 {
    ret 6
  }
  if f64::parse("1.").or_panic_with_message("expected a value") != 1.0 {
    ret 7
  }
  if f64::parse(".5").or_panic_with_message("expected a value") != 0.5 {
    ret 8
  }
  if f64::parse("+0.25").or_panic_with_message("expected a value") != 0.25 {
    ret 9
  }
  if f64::parse("x").is_some() {
    ret 10
  }
  if f64::parse("").is_some() {
    ret 11
  }
  if f64::parse("1.2.3").is_some() {
    ret 12
  }
  s := format("2.5")
  if f64::parse(s.as_str()).or_panic_with_message("expected a value") != 2.5 {
    ret 13
  }
  ret 0
}
