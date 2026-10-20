// An owning closure's environment travels with the value: a second
// binding takes it, so calling the first again would read the storage
// the move gave away.
fn main() -> i32 {
  s := String::from("abc")
  g := [s]() -> s.byte(0) as u64
  h := g
  ret (g() + h()) as i32
}
