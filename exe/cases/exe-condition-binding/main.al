// `if PAT := EXPR` enters when the pattern matches, the refutable
// form the language wants instead of `if let`.
fn none() -> Option<i32> {
  ret Option::None
}

fn main() -> i32 {
  o := Option::Some(3)
  if Option::Some(v) := o {
    if v != 3 {
      ret 1
    }
  } else {
    ret 2
  }
  n := none()
  if Option::Some(v) := n {
    ret 3
  }
  mut count := 0
  while Option::Some(v) := Option::Some(1) {
    if v != 1 {
      ret 4
    }
    count = count + 1
    if count > 3 {
      break
    }
  }
  if count != 4 {
    ret 5
  }
  ret 0
}
