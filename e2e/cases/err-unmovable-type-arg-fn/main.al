// A type argument is Movable, and a function instantiation checks its
// own rather than passing through the nominal path (ADR-0085). The
// argument here is inferred rather than spelled.
struct Pinned {
  held: Anchor,
}

fn take<T>(x: T) -> i32 {
  ret 1
}

fn main() -> i32 {
  p := Pinned { held: Anchor { held: () } }
  ret take(p)
}
