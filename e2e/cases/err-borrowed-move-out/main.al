// A by-reference binding is a shared borrow: moving the referent out
// through it (`*s`) stays refused.

enum E { A(String), B }

fn f(e: &E) -> String {
  ret match *e { E::A(s) => *s, E::B => format("") }
}

fn main() -> i32 {
  e := E::A(format("x"))
  ret f(&e).len() as i32 - 1
}
