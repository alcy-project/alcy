// Calling an unsafe function is an operation the gate covers, and the
// call site has to name it.
unsafe fn poke() {}

fn main() {
  poke()
}
