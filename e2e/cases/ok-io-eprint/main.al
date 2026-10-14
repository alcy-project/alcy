// `eprint` and `eprintln` write stderr; `print` and `println` stdout.
fn main() -> i32 {
  print("out ")
  println("line")
  eprint("err ")
  eprintln("line")
  eprintln("value {}", 7)
  ret 0
}
