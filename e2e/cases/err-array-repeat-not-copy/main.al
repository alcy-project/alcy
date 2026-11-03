// A repeat stores the one value it evaluated in every slot.
// Duplicating a value is observable only for a Copy element: a
// destructor would run once per slot over storage holding the value
// once.

struct Owner {
  n: i32,
}

impl Owner {
  fn drop(self: Owner) {
    println("DROP {}", self.n)
  }
}

fn main() -> i32 {
  a := [Owner { n: 3 }; 2]
  ret a.len() as i32
}
