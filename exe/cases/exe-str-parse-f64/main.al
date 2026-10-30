// A decimal float folds its digits into one numerator and one division,
// with the integer part converted apart from the fraction: one division
// of the whole numerator rounds twice, so a value past 2^53 answered a
// step away from the literal.
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
  // Past 2^53 an integral value keeps its integer part: the split
  // leaves one rounding, where the single division left two.
  exact := f64::parse("9007199254740980.0").or_panic_with_message("expected a value")
  if exact != 9007199254740980.0 {
    ret 14
  }
  rounded := f64::parse("9007199254740993.0").or_panic_with_message("expected a value")
  if rounded != 9007199254740992.0 {
    ret 15
  }
  ret 0
}
