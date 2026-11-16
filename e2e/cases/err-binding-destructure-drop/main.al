// The same rule holds for an irrefutable binding: moving a field out of
// the destructor's value leaves it with a hole.

struct Holder {
  s: String,
  id: i32,
}

impl Holder {
  fn drop(self: Self) {
    println("DROP {}", self.id)
  }
}

fn main() {
  h := Holder { s: String::from("abc"), id: 1 }
  Holder { s: x, id: _ } := h
  println("[{}]", x)
}
