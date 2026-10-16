// An owning temporary borrowed as a call argument or a binding becomes
// an unnamed local, so it is read and retired once.

fn takes(v: &String) -> usize {
  ret v.len()
}

fn main() -> i32 {
  if takes(&format("y")) != 1 {
    ret 1
  }
  if takes(&format("abc")) != 3 {
    ret 2
  }
  r := &format("yz")
  if r.len() != 2 {
    ret 3
  }
  ret 0
}
