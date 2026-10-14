// A guard tests after the arm binds and falls through when it is
// false; a guarded arm does not cover a case.

fn classify(n: i32) -> i32 {
  ret match n {
    x if x < 0 => 0,
    0 => 1,
    _ => 2,
  }
}

fn sign(o: Option<i32>) -> i32 {
  ret match o {
    Option::Some(v) if v > 0 => 1,
    Option::Some(v) if v < 0 => 2,
    Option::Some(_) => 3,
    Option::None => 4,
  }
}

fn either(n: i32) -> i32 {
  ret match n {
    1 | 2 if n == 2 => 10,
    _ => 20,
  }
}

fn main() -> i32 {
  if classify(5) != 2 {
    ret 1
  }
  if classify(0) != 1 {
    ret 2
  }
  if classify(0 - 1) != 0 {
    ret 3
  }
  if sign(Option::Some(5i32)) != 1 {
    ret 4
  }
  if sign(Option::Some(0 - 5i32)) != 2 {
    ret 5
  }
  if sign(Option::Some(0i32)) != 3 {
    ret 6
  }
  if sign(Option::None) != 4 {
    ret 7
  }
  if either(1) != 20 {
    ret 8
  }
  if either(2) != 10 {
    ret 9
  }
  ret 0
}
