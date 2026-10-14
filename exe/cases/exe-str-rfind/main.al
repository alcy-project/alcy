// `rfind` mirrors `find` from the right, including the empty needle.
fn main() -> i32 {
  if "banana".rfind("na").unwrap() != 4 { ret 1 }
  if "banana".rfind("an").unwrap() != 3 { ret 2 }
  if "banana".rfind("zz").is_some() { ret 3 }
  if "abc".rfind("").unwrap() != 3 { ret 4 }
  if "abc".rfind("abcd").is_some() { ret 5 }
  s := format("a.b.c")
  if s.rfind(".").unwrap() != 3 { ret 6 }
  if s.find(".").unwrap() != 1 { ret 7 }
  ret 0
}
