// Folding a checked `+` deletes the overflow it reports, so the passes
// fold one only where the folded result settles that the check stays
// silent (ADR-0068).
fn main() -> i32 {
  a := 2147483647i32
  ret a + 1
}
