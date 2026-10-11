// `format` builds the expansion in an owned `String` on the heap, and
// the line reaches `println` as a `str`.
fn main() {
  line := format("{} + {} = {}", 20, 35, 20 + 35)
  println("{}", line.as_str())
}
