// The overshift check reads the count the source wrote, past the mask
// lowering put in front of the shift (ADR-0068).
fn main() -> i32 {
  n := 33i32
  ret 1i32 << n
}
