// `Option`'s first method set: an eager default, a lazy default, and a
// predicate that keeps the value (ADR: Option and Iterator APIs).

fn main() -> i32 {
  a := Option::<i32>::Some(41)
  none: Option<i32> := Option::None
  if none.unwrap_or(7) != 7 {
    ret 1
  }
  if a.unwrap_or(0) != 41 {
    ret 2
  }
  f := none.or_else(() -> Option::Some(9))
  if f.unwrap_or(0) != 9 {
    ret 3
  }
  pos := a.filter((v: &i32) -> *v > 0)
  if pos.unwrap_or(0) != 41 {
    ret 4
  }
  neg: Option<i32> := Option::Some(0i32 - 1)
  if neg.filter((v: &i32) -> *v > 0).is_some() {
    ret 5
  }
  ret 0
}
