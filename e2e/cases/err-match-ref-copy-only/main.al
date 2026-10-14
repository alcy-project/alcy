// Matching over a borrowed value whose type carries a non-`Copy`
// payload is refused as a whole: the scrutinee is materialized before
// any pattern is read, and that copy would be ended twice. The refusal
// is what the by-reference binding slice retires.
enum E { A(String), B(i32) }

fn tag(e: &E) -> i32 {
  ret match *e {
    E::B(n) => n,
    E::A(_) => 0,
  }
}

fn main() -> i32 {
  mut t := String::new()
  t.push_str("hi")
  e := E::A(t)
  ret tag(&e)
}
