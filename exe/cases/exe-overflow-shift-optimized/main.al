// The shift check reads the count through the mask lowering puts under
// it, so the passes must leave that register alone: folding it away left
// an optimized build computing an out-of-range shift (ADR-0068).
fn main() -> i32 {
  n := 33i32
  ret 1i32 << n
}
