// A hosted program sees at least its own name in the arguments.
fn main() -> i32 {
  if args_len() < 1 { ret 1 }
  ret 0
}
