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

struct Sack {
  items: Vec<Payload>,
  n: i32,
}

enum Slot {
  Full(Bag, i32),
  Empty,
}

fn bag(tag: i32) -> Bag {
  mut b := Bag { items: Vec::<i32>::new(), n: tag }
  b.items.push(tag)
  ret b
}

fn sack(tag: i32) -> Sack {
  mut s := Sack { items: Vec::<Payload>::new(), n: tag }
  s.items.push(Payload { id: tag })
  ret s
}

fn main() {
  // The literal after the binding fails, so nothing leaves the bag.
  held := bag(1)
  match held {
    Bag { items: v, n: 5 } => println("wrong bag {}", v.len()),
    _ => println("bag kept"),
  }
  // A variant payload that fails its literal leaves the enum whole.
  slot := Slot::Full(bag(3), 9)
  match slot {
    Slot::Full(b, 4) => println("wrong slot {}", b.n),
    Slot::Empty => println("wrong empty"),
    _ => println("slot kept"),
  }
  // A struct pattern nested in a tuple waits with the tuple.
  pair := (bag(5), 7)
  match pair {
    (Bag { items: v, n: 5 }, 8) => println("wrong pair {}", v.len()),
    _ => println("pair kept"),
  }
  // A condition pattern that fails keeps the scrutinee.
  cond := bag(6)
  if Bag {
    items: v, n: 4
  } := cond {
    println("wrong cond {}", v.len())
  } else {
    println("cond kept")
  }
  // An arm that matches owns what it bound and ends it once.
  taken := sack(2)
  match taken {
    Sack { items: v, n: 2 } => println("sack took {}", v.len()),
    _ => println("wrong sack"),
  }
  println("done")
}
