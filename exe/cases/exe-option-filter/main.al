// `Option::filter` over a non-Copy payload: the `if` inside moves the
// payload on one arm and ends it on the other, which the borrow check
// accepts now that a placed end is an authorized boundary. The WD004
// warning for the option's own end retires with the payload glue.

fn is_empty(s: &String) -> bool { ret s.is_empty() }

fn main() -> i32 {
  o := Option::Some(String::new())
  f := o.filter(is_empty)
  match f {
    Option::Some(_) => { }
    Option::None => { ret 1 }
  }
  ret 0
}
