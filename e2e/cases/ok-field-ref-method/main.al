// A reference to a field carries the field's storage copy, and a
// method call looks through it. Moving another field is disjoint.
struct S { a: String, b: String }
fn main() -> i32 {
  s := S { a: format("ab"), b: format("cde") }
  r := &s.a
  t := s.b
  if r.len() != 2 { ret 1 }
  if t.len() != 3 { ret 2 }
  ret 0
}
