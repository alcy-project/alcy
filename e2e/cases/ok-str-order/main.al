// Strings order byte by byte through `cmp`, and `sort` uses it.
fn main() -> i32 {
  if !"a".cmp(&"b").is_lt() { ret 1 }
  if !"b".cmp(&"a").is_gt() { ret 2 }
  if !"a".cmp(&"a").is_eq() { ret 3 }
  if !"ab".cmp(&"b").is_lt() { ret 4 }
  if !"abc".cmp(&"ab").is_gt() { ret 5 }
  mut names := Vec::<String>::new()
  names.push(format("b"))
  names.push(format("a"))
  names.push(format("ab"))
  names.sort()
  if names.at(0).unwrap().as_str() != "a" { ret 6 }
  if names.at(1).unwrap().as_str() != "ab" { ret 7 }
  if names.at(2).unwrap().as_str() != "b" { ret 8 }
  ret 0
}
