// A tuple index may be followed by a member call.
fn main() -> i32 {
  t := (format("hi"), 1)
  if t.0.len() != 2 { ret 1 }
  if t.0.as_str() != "hi" { ret 2 }
  if t.1 != 1 { ret 3 }
  ret 0
}
