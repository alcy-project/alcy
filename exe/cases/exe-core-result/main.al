// Exercises the core `Option` and `Result` enums: construction,
// `?` propagation, and the ordinary methods declared in core.
fn lookup(x: i32) -> Result<i32, str> {
  if x > 0 {
    ret Result::Ok(x)
  }
  ret Result::Err("not found")
}

fn bounded(x: i32) -> Result<i32, str> {
  v := lookup(x)?
  if v > 100 {
    ret Result::Err("too large")
  }
  ret Result::Ok(v * 2)
}

fn first_present(x: i32) -> Option<i32> {
  if x > 0 {
    ret Option::Some(x)
  }
  ret Option::None
}

fn double(x: i32) -> Option<i32> {
  v := first_present(x)?
  ret Option::Some(v * 2)
}

fn main() -> i32 {
  mut bad := 0
  if bounded(21).unwrap() != 42 {
    bad = 1
  }
  if bounded(0).is_ok() {
    bad = 2
  }
  if !bounded(200).is_err() {
    bad = 3
  }
  if bounded(0).or(7) != 7 {
    bad = 4
  }
  if bounded(21).expect("unused") != 42 {
    bad = 5
  }
  if double(21).unwrap() != 42 {
    bad = 6
  }
  if double(0).or(-1) != -1 {
    bad = 7
  }
  if !first_present(-1).is_none() {
    bad = 8
  }
  if !first_present(1).is_some() {
    bad = 9
  }
  ret bad
}
