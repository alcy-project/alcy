// A pattern that binds takes its value out of the scrutinee only once
// every test of the pattern has passed, so an arm that fails leaves the
// scrutinee whole for its owner to end (docs/spec/values.md).

struct Payload {
  id: i32,
}

impl Payload {
  fn drop(self: Self) {
    println("DROP {}", self.id)
  }
}

struct Bag {
  items: Vec<i32>,
  n: i32,
}

struct Holder {
  items: Vec<Payload>,
  n: i32,
}

fn bag(tag: i32) -> Bag {
  mut b := Bag { items: Vec::<i32>::new(), n: tag }
  b.items.push(tag)
  ret b
}

fn holder(tag: i32) -> Holder {
  mut h := Holder { items: Vec::<Payload>::new(), n: tag }
  h.items.push(Payload { id: tag })
  ret h
}

fn main() {
  held := bag(1)
  match held {
    Bag { items: v, n: 5 } => println("wrong {}", v.len()),
    _ => println("kept"),
  }
  taken := holder(2)
  match taken {
    Holder { items: v, n: 2 } => println("took {}", v.len()),
    _ => println("wrong"),
  }
  println("done")
}
