// A constant expression that indexes past an array's end is a program
// error, not an operation the lowering cannot express: it names the
// rule the way a checked build refuses the same index at runtime.
comp fn at(comp i: i32) -> i32 {
  ret comp { [10, 20, 30][i] }
}

fn main() {
  const x := at(9)
  _ := x
}
