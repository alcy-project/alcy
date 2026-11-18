// A value reached through one of the receiver's fields is not a field of
// the receiver: the inner type's own `drop` would run on the hole the move
// leaves (docs/spec/values.md).
struct Inner {
  sub: Vec<i32>,
  tag: i32,
}

impl Inner {
  fn drop(self: Inner) {
    println("DROP inner {}", self.tag)
  }
}

struct Bag {
  inner: Inner,
}

impl Bag {
  fn drop(self: Bag) {
    moved := self.inner.sub
    println("moved {}", moved.len())
  }
}

fn main() {
  mut b := Bag { inner: Inner { sub: Vec::<i32>::new(), tag: 7 } }
  b.inner.sub.push(3)
  println("built")
}
