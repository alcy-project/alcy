// A guard runs before the arm commits, so a by-value binding of a
// non-`Copy` value would be taken when the guard runs; a false guard
// falls through to the next arm with the value already gone. Matching
// a reference binds by reference instead.
enum E {
  A(String),
  B,
}

fn main() -> i32 {
  e := E::A(String::from("hello"))
  match e {
    E::A(s) if s.len_bytes() > 3 => {
      ret s.len_bytes() as i32
    }
    E::A(s) => {
      ret 0
    }
    E::B => {
      ret 0
    }
  }
}
