// Replacing a map entry hands the old value back; discarding the answer
// ends it, and the map keeps the new one.

struct P {
  id: i32,
}

impl P {
  fn drop(self: Self) {
    println("DROP {}", self.id)
  }
}

fn main() {
  mut m := HashMap::<String, P>::new()
  _ := m.insert(String::from("a"), P { id: 1 })
  _ := m.insert(String::from("a"), P { id: 2 })
  println("after")
}
