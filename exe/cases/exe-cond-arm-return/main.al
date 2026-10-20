// A pattern condition moves the payload it binds out of the scrutinee.
// When the then-arm returns from inside, the move still has to be
// recorded before the arm's body: otherwise the scrutinee's drop runs
// on the payload the binding took, a double free in safe code.
fn main() -> i32 {
  o := Option::Some(String::from("hello"))
  if Option::Some(s) := o {
    ret s.len() as i32
  }
  ret 0
}
