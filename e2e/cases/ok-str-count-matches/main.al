// `count_matches` counts non-overlapping occurrences.
fn main() -> i32 {
  if "abcabc".count_matches("ab") != 2 { ret 1 }
  if "abc".count_matches("x") != 0 { ret 2 }
  if "aaa".count_matches("aa") != 1 { ret 3 }
  if "abc".count_matches("") != 0 { ret 4 }
  if "banana".count_matches("na") != 2 { ret 5 }
  ret 0
}
