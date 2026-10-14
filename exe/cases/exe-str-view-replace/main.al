// A view can replace too, so a literal needs no binding first.
fn main() -> i32 {
  r1 := "a.b.c".replace(".", "-")
  if r1.as_str() != "a-b-c" { ret 1 }
  r2 := "hello".replace("l", "LL")
  if r2.as_str() != "heLLLLo" { ret 2 }
  r3 := "abc".replace("x", "y")
  if r3.as_str() != "abc" { ret 3 }
  r4 := "abc".replace("", "-")
  if r4.as_str() != "abc" { ret 4 }
  ret 0
}
