// The bool niche's lowering, not only its layout: `Some(true)`,
// `Some(false)`, and `None` share one byte, so a match reads the
// discriminant from the byte and the payload by narrowing it back to a
// bool. The layout was pinned when the niche landed; this is the
// execution case for the code that uses it.

fn main() -> i32 {
  mut hits := 0

  some_true: Option<bool> := Option::Some(true)
  if !some_true.is_some() || some_true.is_none() {
    ret 1
  }
  match some_true {
    Option::Some(v) => {
      if !v {
        ret 2
      }
      hits += 1
    }
    Option::None => {
      ret 3
    }
  }

  some_false: Option<bool> := Option::Some(false)
  if !some_false.is_some() {
    ret 4
  }
  match some_false {
    Option::Some(v) => {
      if v {
        ret 5
      }
      hits += 1
    }
    Option::None => {
      ret 6
    }
  }

  none: Option<bool> := Option::None
  if none.is_some() || !none.is_none() {
    ret 7
  }
  match none {
    Option::Some(v) => {
      ret 8
    }
    Option::None => {
      hits += 1
    }
  }

  // The value form reads the payload without a match: `or` takes the
  // byte-narrowed bool.
  if some_true.or(false) != true || none.or(true) != true {
    ret 9
  }

  ret hits - 3
}
