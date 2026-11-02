// A tuple slot is a place of its own: a store over it ends the value it
// replaces, and a slot moved out on one path ends behind a run-time flag
// rather than twice or not at all.

struct Probe {
  tag: i32,
}

impl Probe {
  fn drop(self: Probe) {
    println("DROP {}", self.tag)
  }
}

fn store_over_slot() {
  mut pair := (Probe { tag: 1 }, Probe { tag: 2 })
  pair.0 = Probe { tag: 3 }
  println("after store")
}

fn slot_moved_out(pick: i32) {
  mut pair := (Probe { tag: 4 }, Probe { tag: 5 })
  if pick == 0 {
    mut taken := pair.0
    println("taken {}", taken.tag)
  }
  pair.0 = Probe { tag: 6 }
  println("after maybe store")
}

fn slot_moved_out_at_exit(pick: i32) {
  mut pair := (Probe { tag: 7 }, Probe { tag: 8 })
  if pick == 0 {
    mut taken := pair.0
    println("taken {}", taken.tag)
  }
  println("after maybe exit")
}

fn main() -> i32 {
  store_over_slot()
  slot_moved_out(0)
  slot_moved_out_at_exit(0)
  ret 0
}
