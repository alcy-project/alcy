// A value whose type declares a destructor cannot be taken apart in a
// pattern, even when every bound field is Copy.

struct Payload {
  id: i32,
  tag: i32,
}

impl Payload {
  fn drop(self: Self) {
    println("DROP {}", self.id)
  }
}

fn main() {
  q := Payload { id: 5, tag: 6 }
  match q {
    Payload { id: x, tag: _ } => println("x {}", x),
  }
  println("after")
}
