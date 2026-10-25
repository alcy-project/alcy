use acme_types::thing::Thing;

pub spec Tagged {
  fn tag(self: &Self) -> i32;
}

// The spec lives here, so this package may implement it for a type
// that belongs to another package (ADR-0090).
impl Tagged for Thing {
  fn tag(self: &Self) -> i32 {
    ret self.n
  }
}
