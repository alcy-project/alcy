// Non-overlapping occurrences; an empty needle counts the gaps.
fn main() -> i32 {
  if "banana".count("an") != 2 { ret 1 }
  if "banana".count("a") != 3 { ret 2 }
  if "banana".count("na") != 2 { ret 3 }
  if "banana".count("zz") != 0 { ret 4 }
  if "abc".count("") != 4 { ret 5 }
  if "".count("") != 1 { ret 6 }
  if "".count("x") != 0 { ret 7 }
  if "aaa".count("aa") != 1 { ret 8 }
  s := format("a.b.c")
  if s.count(".") != 2 { ret 9 }
  ret 0
}
