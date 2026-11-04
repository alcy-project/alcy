// A format argument that calls a method renders through `Display`, on
// both backends.
fn main() {
  println("[{}]", "ab".to_ascii_uppercase())
  println("[{}]", "a-b".replace("-", "+"))
}
