// A wildcard arm over a borrowed scrutinee: the match reads through the
// reference and the referent stays its owner's to end once.

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
  r := &e
  match *r {
    _ => println("wild"),
  }
  println("after")
}
