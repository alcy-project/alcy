// The `f` conversion is optional sugar, and the analyzer asks for it:
// `{:.2}` is the same piece as `{:.2f}`, which says what it formats
// without reading the argument's type beside it.
fn main() {
  println("{:.2}", 3.14159)
  println("{:.2f}", 3.14159)
  ret
}
