// A unit variant names its instance with a turbofish of its own, the
// way a tuple variant names its own with the payload.
fn main() -> i32 {
  n := Option::<i32>::None
  if n.is_some() { ret 1 }
  e := Option::<str>::None
  if e.is_some() { ret 2 }
  ret 0
}
