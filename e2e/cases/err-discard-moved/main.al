// A wildcard discard consumes the place, so a second use is refused.
fn main() -> i32 {
  s := format("x")
  t := s
  _ := s
  ret t.len() as i32
}
