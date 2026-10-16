// A slot inside a map instance infers from the map argument.
fn entries_of<V>(m: &Map<V>) -> usize {
  ret m.len()
}

fn main() -> i32 {
  mut m := Map::<i32>::new()
  _ := m.insert("a", 1)
  _ := m.insert("b", 2)
  if entries_of(&m) != 2 {
    ret 1
  }
  ret 0
}
