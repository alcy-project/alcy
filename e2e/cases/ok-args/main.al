// The argument values are readable; the program name is first.
fn main() -> i32 {
  if args_len() < 1 {
    ret 1
  }
  match arg_at(0) {
    Option::Some(name) => {
      if name.len() < 1 {
        ret 2
      }
    }
    Option::None => {
      ret 3
    }
  }
  if arg_at_str(0).or_panic("the program name is in the arguments").len_bytes() < 1 {
    ret 4
  }
  if arg_at(9999).is_some() {
    ret 5
  }
  if arg_at_str(9999).is_some() {
    ret 6
  }
  ret 0
}
