// A match and a method read a field whose type is an enum.
struct S {
  b: Option<i32>,
  c: Option<i32>,
}

fn main() -> i32 {
  s := S { b: Option::Some(3), c: Option::None }
  match s.b {
    Option::Some(v) => {
      if v != 3 {
        ret 1
      }
    }
    Option::None => {
      ret 2
    }
  }
  match s.c {
    Option::Some(_) => {
      ret 3
    }
    Option::None => {}
  }
  if s.b.or_panic_with_message("expected a value") != 3 {
    ret 4
  }
  if s.c.is_some() {
    ret 5
  }
  ret 0
}
