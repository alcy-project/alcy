// Collect a map's keys into a vector and order them, the shape a
// front end uses to report declarations in name order.
fn main() -> i32 {
  mut m := Map::<i32>::new()
  _ := m.insert("b", 2)
  _ := m.insert("a", 1)
  mut names := Vec::<String>::new()
  for key in m.keys() {
    names.push(String::from(key))
  }
  if names.len() != 2 { ret 1 }
  names.sort()
  if names.at(0).unwrap().as_str() != "a" { ret 2 }
  if names.at(1).unwrap().as_str() != "b" { ret 3 }
  ret 0
}
