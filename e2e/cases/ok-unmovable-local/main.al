// An unmovable value is built in place, read through borrows, and
// ended in place.
struct Pinned {
  held: Anchor,
  value: i32,
}

fn main() -> i32 {
  p := Pinned { held: Anchor { held: () }, value: 7 }
  q := &p
  if q.value != 7 {
    ret 1
  }
  ret 0
}
