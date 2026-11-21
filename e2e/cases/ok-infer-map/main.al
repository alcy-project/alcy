// A slot inside a map instance infers from the map argument.
fn entries_of<V>(m: &HashMap < String, V >) -> usize {
  ret m.len()
}

fn main() -> i32 {
  mut m := HashMap::<String, i32>::new()
  _ := m.insert(String::from("a"), 1)
  _ := m.insert(String::from("b"), 2)
  if entries_of(&m) != 2 {
    ret 1
  }
  ret 0
}
