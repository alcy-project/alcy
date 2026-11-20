// A pattern's wildcard ends the value it discards, wherever the value
// came from: a struct field, a tuple slot, or an enum payload.

struct Tags {
  ids: Vec<i32>,
}

impl Tags {
  fn drop(self: Self) {
    _ := self.ids
    print("drop tags\n")
  }
}

struct S {
  items: Vec<i32>,
  tags: Tags,
}

enum E {
  A(Tags),
  B,
}

fn main() -> i32 {
  mut s := S { items: Vec::<i32>::new(), tags: Tags { ids: Vec::<i32>::new() } }
  s.items.push(7)
  s.tags.ids.push(1)
  s.tags.ids.push(2)
  match s {
    S { items: v, tags: _ } => print("items\n"),
  }
  mut t := (Tags { ids: Vec::<i32>::new() }, 5i32)
  t.0.ids.push(3)
  match t {
    (_, n) => print("n {}\n", n),
  }
  mut e := E::A(Tags { ids: Vec::<i32>::new() })
  match e {
    E::A(_) => print("A\n"),
    E::B => print("B\n"),
  }
  print("done\n")
  ret 0
}
