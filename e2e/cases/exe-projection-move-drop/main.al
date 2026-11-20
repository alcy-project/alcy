// A field read out of a temporary leaves the aggregate: the field
// moves on, and the fields the read left behind end with the block.

struct Probe {
  id: i32,
}

impl Probe {
  fn drop(self: Probe) {
    println("DROP {}", self.id)
  }
}

fn pair(tag: i32) -> (String, Probe) {
  ret (String::from("kept"), Probe { id: tag })
}

fn take(probe: Probe) {
  println("take {}", probe.id)
}

fn main() -> i32 {
  a := pair(1i32).0
  println("a {}", a)
  take(pair(2i32).1)
  ret 0
}
