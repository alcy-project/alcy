// The argument list is a vector of owned strings.
fn main() -> i32 {
  a := args()
  if a.len() < 1 {
    ret 1
  }
  if a.at(0).unwrap().len() < 1 {
    ret 2
  }
  ret 0
}
