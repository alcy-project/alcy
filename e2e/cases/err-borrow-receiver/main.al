// An implicit reborrow at a method receiver is a loan too, whether or
// not the source writes a `&`. The method here hands back a reference
// derived from `&self`, so the store has to conflict with it.
struct Box { n: i32 }

impl Box {
  fn get(self: &Self) -> &i32 {
    ret &self.n
  }
}

fn main() -> i32 {
  mut x := Box { n: 1 }
  r := x.get()
  x.n = 9
  ret *r - 9
}
