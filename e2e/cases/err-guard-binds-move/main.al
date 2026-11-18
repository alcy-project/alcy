// A guard must not bind a non-Copy payload by value: a false guard falls
// through to the next arm after the value is taken.

enum E {
  A(String),
  B,
}

fn main() {
  e := E::A(String::from("abc"))
  match e {
    E::A(s) if s.len_bytes() > 0 => println("[{}]", s),
    _ => println("other"),
  }
  println("after")
}
