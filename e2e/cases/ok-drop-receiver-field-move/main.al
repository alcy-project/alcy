// A destructor's body may move a field of its receiver into a binding:
// the binding owns the value from there and ends it where it stands
// (docs/spec/values.md, ADR-0137).

struct Payload {
  id: i32,
}

impl Payload {
  fn drop(self: Self) {
    println("DROP payload {}", self.id)
  }
}

struct Bag {
  payloads: Vec<Payload>,
  spare: Vec<Payload>,
  keep: bool,
}

impl Bag {
  fn drop(self: Self) {
    println("DROP bag keep={}", self.keep)
    if self.keep {
      moved := self.payloads
      println("moved {}", moved.len())
    } else {
      unsafe { drop_in_place::<Vec<Payload>>(&mut self.payloads) }
      println("ended in place")
    }
    unsafe { drop_in_place::<Vec<Payload>>(&mut self.spare) }
  }
}

fn bag(keep: bool, tag: i32) -> Bag {
  mut b := Bag { payloads: Vec::<Payload>::new(), spare: Vec::<Payload>::new(), keep: keep, }
  b.payloads.push(Payload { id: tag })
  b.spare.push(Payload { id: tag + 10 })
  ret b
}

fn main() {
  kept := bag(true, 1)
  placed := bag(false, 2)
  println("built {} {}", kept.keep, placed.keep)
}
