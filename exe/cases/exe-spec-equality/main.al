// `==` and `!=` reach `PartialEq` over the sealed specs (ADR-0053).

fn main() -> i32 {
  mut a := Vec::<i32>::new()
  a.push(1)
  a.push(2)
  mut b := Vec::<i32>::new()
  b.push(1)
  b.push(2)
  if !(a == b) {
    ret 1
  }
  if a != b {
    ret 2
  }
  b.push(3)
  if a == b {
    ret 3
  }
  if !(a != b) {
    ret 4
  }

  mut s := String::new()
  s.push_str("hi")
  mut t := String::new()
  t.push_str("hi")
  if !(s == t) {
    ret 5
  }
  t.push_str("!")
  if s == t {
    ret 6
  }
  if !(s != t) {
    ret 7
  }

  // A generic container compares its elements through `PartialEq` in
  // turn: `Vec<String>` needs `String`'s implementation.
  mut sa := Vec::<String>::new()
  mut e := String::new()
  e.push_str("x")
  sa.push(e)
  mut sb := Vec::<String>::new()
  mut f := String::new()
  f.push_str("x")
  sb.push(f)
  if !(sa == sb) {
    ret 8
  }
  // End the elements explicitly: dropping a `Vec` does not end what it
  // holds (see deferred.md), and the sanitized run checks for leaks.
  sv := sa.pop().unwrap()
  sw := sb.pop().unwrap()
  ret 0
}
