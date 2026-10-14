// A bare statement discards too, and consumes the place.
fn main() -> i32 {
  s := format("x")
  t := s
  s
  ret t.len() as i32
}
