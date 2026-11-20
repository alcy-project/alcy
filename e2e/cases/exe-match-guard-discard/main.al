// A guarded arm tests before it commits: what the arm discards stays
// with the arm's own path, and the arm that matches ends it once.

struct Tags {
  ids: Vec<i32>,
}

impl Tags {
  fn drop(self: Self) {
    _ := self.ids
    print("drop tags\n")
  }
}

enum Wrapped {
  A(Tags),
  B,
}

fn make() -> Tags {
  ret Tags { ids: Vec::<i32>::new() }
}

fn wrap() -> Wrapped {
  ret Wrapped::A(make())
}

fn main() -> i32 {
  // A wildcard whose guard falls through leaves the value to the next arm.
  s := make()
  match s {
    _ if false => print("one\n"),
    _ => print("two\n"),
  }
  // The payload a guarded arm discards stays with it until it commits.
  e := wrap()
  match e {
    Wrapped::A(_) if false => print("a\n"),
    Wrapped::A(v) => {
      print("b\n")
      _ := v
    }
    Wrapped::B => print("c\n"),
  }
  // A wildcard arm discards the value a call leaves behind the same way.
  match make() {
    _ => print("wild\n"),
  }
  print("end\n")
  ret 0
}
