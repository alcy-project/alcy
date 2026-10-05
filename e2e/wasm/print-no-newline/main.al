// `print` keeps the line open, so the newline can only come from the last
// call: a stray one anywhere shows up as a mismatch.
fn main() {
  print("AB")
  print("CD")
  println("")
}
