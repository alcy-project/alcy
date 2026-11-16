// An enum with no destructor of its own still ends the variant's
// payload after a binding owns it.

struct Payload {
  id: i32,
}

impl Payload {
  fn drop(self: Self) {
    println("DROP {}", self.id)
  }
}

enum E {
  A(Payload),
  B,
}

fn main() {
  e := E::A(Payload { id: 1 })
  match e {
    E::A(p) => println("p {}", p.id),
    E::B => println("b"),
  }
  println("after")
}
