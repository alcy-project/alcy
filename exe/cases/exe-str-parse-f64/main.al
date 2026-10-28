// A decimal float folds its digits into one numerator and one division,
// with the integer part converted apart from the fraction: one division
// of the whole numerator rounds twice, so a value past 2^53 answered a
// step away from the literal.
fn main() -> i32 {
  if "3.5".parse_f64().or_panic_with_message("expected a value") != 3.5 {
    ret 1
  }
  if "0.5".parse_f64().or_panic_with_message("expected a value") != 0.5 {
    ret 2
  }
  if "-2.25".parse_f64().or_panic_with_message("expected a value") != 0.0f64 - 2.25 {
    ret 3
  }
  if "3.14".parse_f64().or_panic_with_message("expected a value") != 3.14 {
    ret 4
  }
  if "0.1".parse_f64().or_panic_with_message("expected a value") != 0.1 {
    ret 5
  }
  if "1".parse_f64().or_panic_with_message("expected a value") != 1.0 {
    ret 6
  }
  if "1.".parse_f64().or_panic_with_message("expected a value") != 1.0 {
    ret 7
  }
  if ".5".parse_f64().or_panic_with_message("expected a value") != 0.5 {
    ret 8
  }
  if "+0.25".parse_f64().or_panic_with_message("expected a value") != 0.25 {
    ret 9
  }
  if "x".parse_f64().is_some() {
    ret 10
  }
  if "".parse_f64().is_some() {
    ret 11
  }
  if "1.2.3".parse_f64().is_some() {
    ret 12
  }
  s := format("2.5")
  if s.parse_f64().or_panic_with_message("expected a value") != 2.5 {
    ret 13
  }
  // Past 2^53 an integral value keeps its integer part: the split
  // leaves one rounding, where the single division left two.
  exact := "9007199254740980.0".parse_f64().or_panic_with_message("expected a value")
  if exact != 9007199254740980.0 {
    ret 14
  }
  rounded := "9007199254740993.0".parse_f64().or_panic_with_message("expected a value")
  if rounded != 9007199254740992.0 {
    ret 15
  }
  ret 0
}
