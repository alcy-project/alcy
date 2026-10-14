// A match on a borrowed enum reads its discriminant through the
// reference: a wildcard payload is neither loaded nor ended, so the
// value stays whole and can be consulted again.

enum E<T> { A(T), B }

impl<T> E<T> {
  fn has(self: &Self) -> bool {
    ret match *self { E::A(_) => true, E::B => false }
  }
}

struct P { s: String }

impl P {
  fn is_named(self: &Self) -> bool {
    ret match *self { P { s: _ } => true }
  }
}

fn main() -> i32 {
  e: E<String> := E::A(format("x"))
  if !e.has() { ret 1 }
  if !e.has() { ret 2 }
  p := P { s: format("a") }
  if !p.is_named() { ret 3 }
  if !p.is_named() { ret 4 }
  o := Option::Some(format("y"))
  if !o.is_some() { ret 5 }
  if o.is_none() { ret 6 }
  if !o.is_some() { ret 7 }
  n: Option<i32> := Option::None
  if n.is_some() { ret 8 }
  if !n.is_none() { ret 9 }
  r: Result<i32, str> := Result::Ok(7i32)
  if !r.is_ok() { ret 10 }
  if r.is_err() { ret 11 }
  err: Result<i32, str> := Result::Err("bad")
  if !err.is_err() { ret 12 }
  ret 0
}
