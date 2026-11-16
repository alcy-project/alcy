// An if-condition pattern takes the value apart too, so the same rule
// refuses it.

struct Payload {
  id: i32,
}

impl Payload {
  fn drop(self: Self) {
    println("DROP {}", self.id)
  }
}

fn main() {
  q := Payload { id: 5 }
  if Payload { id: x } := q {
    println("x {}", x)
  }
  println("after")
}
