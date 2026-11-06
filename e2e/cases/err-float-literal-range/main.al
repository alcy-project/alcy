// A float literal's digits must stay finite at the type it takes:
// past the largest finite value the nearest one is infinity, which the
// digits do not name.
fn main() -> i32 {
  suffixed := 1e40f32
  plain := 1e400
  annotated: f32 := 1e40
  ret 0
}
