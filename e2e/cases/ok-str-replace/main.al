// `replace` swaps every occurrence in a fresh string. Each result is
// bound so its buffer has an end site.
fn main() -> i32 {
  s1 := format("a.b.c")
  r1 := s1.replace(".", "-")
  if r1.as_str() != "a-b-c" { ret 1 }
  s2 := format("hello")
  r2 := s2.replace("l", "LL")
  if r2.as_str() != "heLLLLo" { ret 2 }
  s3 := format("abc")
  r3 := s3.replace("x", "y")
  if r3.as_str() != "abc" { ret 3 }
  s4 := format("abc")
  r4 := s4.replace("", "-")
  if r4.as_str() != "abc" { ret 4 }
  s5 := format("aaa")
  r5 := s5.replace("a", "aa")
  if r5.as_str() != "aaaaaa" { ret 5 }
  ret 0
}
