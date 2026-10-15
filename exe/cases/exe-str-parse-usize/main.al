// `parse_usize` reads an unsigned machine word from text.
fn main() -> i32 {
  if "42".parse_usize().unwrap() != 42 as usize { ret 1 }
  if "x".parse_usize().is_some() { ret 2 }
  if "-1".parse_usize().is_some() { ret 3 }
  if "".parse_usize().is_some() { ret 4 }
  mut s := format("7")
  if s.parse_usize().unwrap() != 7 as usize { ret 5 }
  ret 0
}
