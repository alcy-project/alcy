// A scope chain: each scope names its parent by position, and a walk
// counts how deep one sits.
struct Scope {
  name: String,
  parent: i32,
}

fn depth_of(scopes: &Vec<Scope>, at: i32) -> i32 {
  mut n := 0
  mut cur := at
  while cur >= 0 {
    n = n + 1
    cur = scopes.at(cur as usize).or_panic_with_message("expected a value").parent
  }
  ret n
}

fn main() -> i32 {
  mut scopes := Vec::<Scope>::new()
  scopes.push(Scope { name: String::from("root"), parent: -1 })
  scopes.push(Scope { name: String::from("fn"), parent: 0 })
  scopes.push(Scope { name: String::from("block"), parent: 1 })
  if depth_of(&scopes, 2) != 3 {
    ret 1
  }
  if depth_of(&scopes, 0) != 1 {
    ret 2
  }
  ret 0
}
