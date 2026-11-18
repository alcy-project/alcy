// The argument list is a vector of owned vectors of bytes.
fn main() -> i32 {
  a := args()
  if a.len() < 1 {
    ret 1
  }
  if a.at(0).or_panic_with_message("expected a value").len() < 1 {
    ret 2
  }
  text := args_str().or_panic_with_message("the arguments are well-formed text")
  if text.len() < 1 {
    ret 3
  }
  ret 0
}
