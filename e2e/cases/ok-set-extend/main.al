// `extend` consumes another set and copies its keys in.
fn main() -> i32 {
  mut a := HashSet::<String>::new()
  _ := a.insert(String::from("x"))
  _ := a.insert(String::from("y"))
  mut b := HashSet::<String>::new()
  _ := b.insert(String::from("y"))
  _ := b.insert(String::from("z"))
  a.extend(b)
  if a.len() != 3 {
    ret 1
  }
  if !a.contains(String::from("x")) {
    ret 2
  }
  if !a.contains(String::from("y")) {
    ret 3
  }
  if !a.contains(String::from("z")) {
    ret 4
  }
  ret 0
}
