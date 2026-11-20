// A store over an element ends the value it replaces, whether the index
// is a constant or a register and whether the array is a local or a
// field of one.

struct Probe {
  tag: i32,
}

impl Probe {
  fn drop(self: Probe) {
    println("DROP {}", self.tag)
  }
}

struct Bag {
  slots: [Probe; 2],
}

fn first() -> usize {
  ret 0
}

fn main() -> i32 {
  mut slots := [Probe { tag: 1 }, Probe { tag: 2 }]
  slots[0] = Probe { tag: 3 }
  slots[first()] = Probe { tag: 4 }
  mut bag := Bag { slots: [Probe { tag: 5 }, Probe { tag: 6 }] }
  bag.slots[0] = Probe { tag: 7 }
  println("after")
  ret 0
}
