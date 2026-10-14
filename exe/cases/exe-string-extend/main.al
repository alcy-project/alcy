// `extend` consumes another string and appends its bytes.
fn main() -> i32 {
  a := format("cd")
  mut b := format("ab")
  b.extend(a)
  if b.as_str() != "abcd" { ret 1 }
  e := format("")
  mut f := format("xy")
  f.extend(e)
  if f.as_str() != "xy" { ret 2 }
  ret 0
}
