// An unmovable value lives where it was built: a move is refused
// (ADR-0085).
struct Pinned {
  held: Anchor,
  value: i32,
}

fn main() -> i32 {
  p := Pinned { held: Anchor { held: () }, value: 7 }
  q := p
  ret q.value
}
