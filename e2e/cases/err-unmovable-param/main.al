// A parameter is Movable, and a value holding an anchor is not
// (ADR-0085).
struct Pinned {
  held: Anchor,
}

fn take(p: Pinned) -> i32 {
  ret 1
}

fn main() -> i32 {
  p := Pinned { held: Anchor { held: () } }
  ret take(p)
}
