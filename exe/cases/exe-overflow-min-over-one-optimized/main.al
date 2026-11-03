// The signed division check reads a select over both operands, so folding
// either leaves the abort comparing values the program never computed
// (ADR-0068).
fn main() -> i32 {
  a := -128i8
  b := -1i8
  ret (a / b) as i32
}
