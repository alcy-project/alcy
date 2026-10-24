// A match through a shared reference binds a payload by reference, so
// the borrow yields a `&String` and the referent stays its owner to
// end.
enum E {
  A(String),
  B,
}

fn f(e: &E) -> i32 {
  r := match *e {
    E::A(s) => s.len_bytes() as i32,
    E::B => 0,
  }
  ret r
}

fn main() -> i32 {
  mut t := String::new()
  t.push_str("hi")
  e := E::A(t)
  ret f(&e) - 2
}
