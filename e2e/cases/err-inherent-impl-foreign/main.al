use acme_types::thing::Thing;

// `Thing` is `types`'s, so an inherent block on it belongs to that
// package, not this one (ADR-0090).
impl Thing {
  fn double(self: &Self) -> i32 {
    ret self.n + self.n
  }
}

fn main() -> i32 {
  ret 0
}
