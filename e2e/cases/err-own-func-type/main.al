// The whole return type is the position a spelling desugars in; one
// nested inside another type waits for its slice, as does a field
// (`struct H { f: fn(...) }`).
fn make() -> Option < fn (i32) -> i32 > {
  ret None
}

fn main() -> i32 {
  ret 0
}
