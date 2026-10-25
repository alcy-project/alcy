// The spec's package implemented it for `Thing`, so the method is
// reachable here through the spec.
use acme_specs::tagged::Tagged;
use acme_types::thing::Thing;

fn main() -> i32 {
  t := Thing { n: 7 }
  if t.tag() != 7 {
    ret 1
  }
  ret 0
}
