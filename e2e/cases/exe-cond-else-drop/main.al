// A pattern condition consumes its scrutinee on either arm: when the
// pattern does not match, the else-arm ends the value in place, so its
// destructor runs rather than leaking with the arm that ignored it.
fn main() -> i32 {
  o := Option::Some(String::from("hello"))
  mut r := 0
  if Option::None := o {
    r = 1
  } else {
    r = 2
  }
  if r != 2 {
    ret 1
  }
  ret 0
}
