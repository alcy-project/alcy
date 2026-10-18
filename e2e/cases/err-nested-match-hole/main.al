// Every variant has an arm, but one payload does not: `Some(Some(v))`
// and `None` leave `Some(None)` with no arm, which used to reach code
// the match never wrote - a segfault on the host and a trap on wasm.
fn describe(x: Option<Option<i32>>) -> i32 {
  match x {
    Option::Some(Option::Some(v)) => { ret v }
    Option::None => { ret 0 }
  }
}

fn main() -> i32 {
  ret describe(Option::Some(Option::None))
}
