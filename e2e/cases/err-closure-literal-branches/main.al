// Two closure literals checked against different signatures are two
// different nominals, and a diagnostic spells each one's signature
// rather than reading as a type that mismatches itself.
fn pick(flag: bool) -> i32 {
  c := if flag { []() -> 1 } else { [](x: i32) -> x + 1 }
  ret c()
}

fn main() -> i32 {
  ret pick(true)
}
