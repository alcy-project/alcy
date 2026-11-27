// Collect a map's keys into a vector and order them, the shape a
// front end uses to report declarations in name order.
fn main() -> i32 {
  mut m := HashMap::<String, i32>::new()
  _ := m.insert(String::from("b"), 2)
  _ := m.insert(String::from("a"), 1)
  mut names := Vec::<String>::new()
  for (key, _) in m.into_iter() {
    names.push(key)
  }
  if names.len() != 2 {
    ret 1
  }
  names.sort()
  if names.at(0).or_panic("expected a value").as_str() != "a" {
    ret 2
  }
  if names.at(1).or_panic("expected a value").as_str() != "b" {
    ret 3
  }
  ret 0
}
