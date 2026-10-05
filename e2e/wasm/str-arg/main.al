// A `str` parameter is two words across the call, and the callee passes
// it on to the runtime unchanged.
fn greet(msg: str) {
  println(msg)
}

fn main() {
  greet("passed through")
}
