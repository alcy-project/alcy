// A binding behind a borrow is a reference: a non-Copy payload is read
// in place, and Option/Result equality works for a non-Copy T.

enum E { A(String), B }

fn slen(e: &E) -> usize {
  ret match *e { E::A(s) => s.len(), E::B => 0 }
}

struct P { s: String }

fn plen(p: &P) -> usize {
  ret match *p { P { s } => s.len() }
}

fn main() -> i32 {
  e := E::A(format("abc"))
  if slen(&e) != 3 { ret 1 }
  if slen(&e) != 3 { ret 2 }
  p := P { s: format("abcd") }
  if plen(&p) != 4 { ret 3 }
  a := Option::Some(format("x"))
  b := Option::Some(format("x"))
  c := Option::Some(format("y"))
  if !(a == b) { ret 4 }
  if a == c { ret 5 }
  n: Option<String> := Option::None
  if a == n { ret 6 }
  if !(n == n) { ret 7 }
  ra: Result<String, i32> := Result::Ok(format("x"))
  rb: Result<String, i32> := Result::Ok(format("x"))
  if !(ra == rb) { ret 8 }
  ret 0
}
