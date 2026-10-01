// Every arm here ends the path with `ret`, so no arm falls through to a
// join and the expression has no value on any path. Written as a trailing
// expression of a non-`()` function - the natural way to say "match, or
// return early" - that is the shape whose lowering read a result slot no
// arm had ever written, which left the block with two terminators.
fn classify(opt: Option<i32>) -> i32 {
  match opt {
    Option::Some(v) => { ret v * 2 }
    Option::None => { ret 0 - 1 }
  }
}

fn pick(x: i32) -> i32 {
  if x > 0 {
    ret 1
  } else {
    ret 0
  }
}

fn pick_let(opt: Option<i32>) -> i32 {
  if Option::Some(v) := opt {
    ret v
  } else {
    ret 0
  }
}

// An or-pattern expands into several arms, so this is the same shape one
// level down from `classify`.
fn either(opt: Option<i32>) -> i32 {
  match opt {
    Option::Some(_) | Option::None => { ret 0 }
  }
}

// An arm whose body is a block that returns through a call.
fn nested(opt: Option<Option<i32>>) -> i32 {
  match opt {
    Option::Some(inner) => {
      ret classify(inner)
    }
    Option::None => { ret 0 - 1 }
  }
}

// One arm falls through, so there is a join and a value. This is the
// case the all-terminated one has to stay distinct from.
fn mixed(opt: Option<i32>) -> i32 {
  match opt {
    Option::Some(v) => { ret v }
    Option::None => 0
  }
}

fn main() -> i32 {
  mut bad := 0
  if classify(Option::Some(21)) != 42 { bad = 1 }
  if classify(Option::None) != 0 - 1 { bad = 2 }
  if pick(5) != 1 { bad = 3 }
  if pick(0 - 5) != 0 { bad = 4 }
  if pick_let(Option::Some(7)) != 7 { bad = 5 }
  if pick_let(Option::None) != 0 { bad = 6 }
  if either(Option::Some(1)) != 0 { bad = 7 }
  if either(Option::None) != 0 { bad = 8 }
  if nested(Option::Some(Option::Some(21))) != 42 { bad = 9 }
  if nested(Option::Some(Option::None)) != 0 - 1 { bad = 10 }
  if nested(Option::None) != 0 - 1 { bad = 11 }
  if mixed(Option::Some(3)) != 3 { bad = 12 }
  if mixed(Option::None) != 0 { bad = 13 }
  ret bad
}