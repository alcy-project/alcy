use acme_specs::tagged::Tagged;
use acme_types::thing::Thing;

// Neither `Thing` nor `Tagged` is declared here, so this package may
// implement neither for the other (ADR-0090).
impl Tagged for Thing {
  fn tag(self: &Self) -> i32 {
    ret self.n
  }
}

fn main() -> i32 {
  ret 0
}
