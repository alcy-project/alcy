// Matching through a reference: the scrutinee is the referent, and an
// arm binding of an aggregate payload owns a slot of its own. The slot
// ends with the arm; a scope-exit drop after the join did not dominate
// it and the emitted IR was invalid.
enum E { A(String), B }

fn f(e: &E) -> i32 {
  r := match *e {
    E::A(s) => s.len() as i32,
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
