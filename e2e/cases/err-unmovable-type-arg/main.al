// A type argument is Movable, and a value holding an anchor is not
struct Pinned {
  held: Anchor,
}

fn main() -> i32 {
  v: Vec<Pinned> := Vec::new()
  ret v.len() as i32
}
