// A variant pattern on an enum whose type declares a destructor is
// refused like a struct pattern: the destructor consumes the value.

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

impl E {
  fn drop(self: Self) {
    println("DROP E")
  }
}

fn main() {
  e := E::A(Payload { id: 1 })
  match e {
    E::A(p) => println("a {}", p.id),
    E::B => println("b"),
  }
  println("after")
}
