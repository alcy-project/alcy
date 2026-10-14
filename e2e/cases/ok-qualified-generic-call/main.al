// A module-qualified name reaches a generic function the way a bare
// one does; the call's turbofish instantiates it.
fn main() -> i32 {
  ret helper::id::<i32>(7) - 7
}
