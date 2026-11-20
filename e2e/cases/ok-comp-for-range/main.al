// A `for` over a range inside a comp function evaluates through the
// range's own Iterator: the cursor is a value struct, so the
// interpreter can advance it and the sum splices as a comp-known int.
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
