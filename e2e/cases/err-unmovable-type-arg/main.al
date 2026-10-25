// A type argument is Movable, and a value holding an anchor is not
// (ADR-0085).
struct Pinned {
  held: Anchor,
}

fn main() -> i32 {
  v: Vec<Pinned> := Vec::new()
  ret v.len() as i32
}
