// `rposition` finds the last accepted index.
fn main() -> i32 {
  a := [1, 2, 3, 2]
  s := a.as_slice()
  if s.rposition((x: &i32) -> *x == 2).or_panic_with_message("expected a value") != 3 {
    ret 1
  }
  if s.rposition((x: &i32) -> *x == 9).is_some() {
    ret 2
  }
  if s.rposition((x: &i32) -> *x == 1).or_panic_with_message("expected a value") != 0 {
    ret 3
  }
  ret 0
}
