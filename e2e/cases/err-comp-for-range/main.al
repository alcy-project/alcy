// A `for` loop in a comp function reports the loop's iterable, not the
// cursor declaration the desugar builds from it: the constraint the
// source states is about the loop, and the constant it would need is the
// one the iterable would produce.
comp fn total() -> i32 {
  mut s := 0
  for i in 0..<4 {
    s += i
  }
  ret s
}

fn main() {
  const x := total()
  _ := x
}
