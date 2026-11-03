// A store over an element ends the value it replaces.

struct Probe {
  tag: i32,
}

impl Probe {
  fn drop(self: Probe) {
    println("DROP {}", self.tag)
  }
}

fn main() -> i32 {
  mut slots := [Probe { tag: 1 }, Probe { tag: 2 }]
  slots[0] = Probe { tag: 3 }
  println("after")
  ret 0
}
