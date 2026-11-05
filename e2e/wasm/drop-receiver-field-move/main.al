// A destructor's body moves a field of its receiver into a binding, and
// the binding ends what it took (docs/spec/values.md, ADR-0137).

struct Payload {
  id: i32,
}

impl Payload {
  fn drop(self: Self) {
    println("DROP {}", self.id)
  }
}

struct Bag {
  items: Vec<Payload>,
}

impl Bag {
  fn drop(self: Self) {
    println("bag")
    moved := self.items
    println("moved {}", moved.len())
  }
}

fn main() {
  mut held := Bag { items: Vec::<Payload>::new() }
  held.items.push(Payload { id: 1 })
  held.items.push(Payload { id: 2 })
  println("built")
}
