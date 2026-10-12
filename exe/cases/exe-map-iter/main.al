// `for` walks a map's entries, keys, and values, and a set's keys.

fn main() -> i32 {
  mut m: Map<i32> := Map::<i32>::new()
  _ := m.insert("a", 1)
  _ := m.insert("b", 2)
  mut total := 0
  mut key_len: usize := 0
  for (k, v) in m.iter() {
    total = total + *v
    key_len = key_len + str_len(k)
  }
  if total != 3 {
    ret 1
  }
  if key_len != 2 {
    ret 2
  }
  mut values := 0
  for v in m.values() {
    values = values + *v
  }
  if values != 3 {
    ret 3
  }

  mut s: Set := Set::new()
  _ := s.insert("x")
  _ := s.insert("y")
  mut set_len: usize := 0
  for k in s.iter() {
    set_len = set_len + str_len(k)
  }
  if set_len != 2 {
    ret 4
  }
  ret 0
}
