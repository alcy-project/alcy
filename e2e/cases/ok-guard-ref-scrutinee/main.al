// A guard over a reference scrutinee binds by reference, so a false
// guard leaves the value where it is.
enum E {
  A(String),
  B,
}

fn main() -> i32 {
  e := E::A(String::from("hello"))
  match &e {
    E::A(s) if s.len() > 100 => { ret 1 }
    E::A(s) => { ret s.len() as i32 }
    E::B => { ret 0 }
  }
}
