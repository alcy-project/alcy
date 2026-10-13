// Matching through a reference: the scrutinee is the referent, and a
// pattern that binds a non-`Copy` payload by value would take it out of
// a value its owner still ends.
enum E { A(String), B }

fn f(e: &E) -> i32 {
  ret match *e {
    E::A(s) => s.len() as i32,
    E::B => 0,
  }
}

fn main() -> i32 {
  mut t := String::new()
  t.push_str("hi")
  e := E::A(t)
  ret f(&e) - 2
}
