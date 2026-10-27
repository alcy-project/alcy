// A type argument is Movable, and a generic method's instantiation
// checks its own rather than passing through the nominal path
// (ADR-0085).
struct Pinned {
  held: Anchor,
}

struct Holder<T> {
  v: T,
}

impl<T> Holder<T> {
  fn count<U>(x: U) -> i32 {
    ret 1
  }
}

fn main() -> i32 {
  p := Pinned { held: Anchor { held: () } }
  ret Holder::<i32>::count(p)
}
