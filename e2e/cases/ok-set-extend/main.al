// `extend` consumes another set and copies its keys in.
fn main() -> i32 {
  mut a := Set::new()
  _ := a.insert("x")
  _ := a.insert("y")
  mut b := Set::new()
  _ := b.insert("y")
  _ := b.insert("z")
  a.extend(b)
  if a.len() != 3 { ret 1 }
  if !a.contains("x") { ret 2 }
  if !a.contains("y") { ret 3 }
  if !a.contains("z") { ret 4 }
  ret 0
}
