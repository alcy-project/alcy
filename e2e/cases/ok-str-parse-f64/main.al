// A decimal float folds its digits into one numerator and one division.
fn main() -> i32 {
  if "3.5".parse_f64().unwrap() != 3.5 { ret 1 }
  if "0.5".parse_f64().unwrap() != 0.5 { ret 2 }
  if "-2.25".parse_f64().unwrap() != 0.0f64 - 2.25 { ret 3 }
  if "3.14".parse_f64().unwrap() != 3.14 { ret 4 }
  if "0.1".parse_f64().unwrap() != 0.1 { ret 5 }
  if "1".parse_f64().unwrap() != 1.0 { ret 6 }
  if "1.".parse_f64().unwrap() != 1.0 { ret 7 }
  if ".5".parse_f64().unwrap() != 0.5 { ret 8 }
  if "+0.25".parse_f64().unwrap() != 0.25 { ret 9 }
  if "x".parse_f64().is_some() { ret 10 }
  if "".parse_f64().is_some() { ret 11 }
  if "1.2.3".parse_f64().is_some() { ret 12 }
  s := format("2.5")
  if s.parse_f64().unwrap() != 2.5 { ret 13 }
  ret 0
}
