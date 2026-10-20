// A match consumes its scrutinee, and the move has to be recorded on
// the path an arm returns from too: without it the scrutinee's drop
// ran on the payload the binding took, a double free in safe code.
enum M {
  S(String),
  N,
}

fn len_of(m: M) -> i32 {
  match m {
    M::S(x) => { ret x.len() as i32 }
    M::N => { ret 0 }
  }
}

fn main() -> i32 {
  if len_of(M::S(String::from("hello"))) != 5 {
    ret 1
  }
  ret 0
}
