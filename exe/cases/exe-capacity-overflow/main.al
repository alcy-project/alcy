// Doubling a capacity near the pointer-width maximum is refused before
// the wrap: a wrapped capacity would name a smaller block than the
// container records, so growth panics instead.
fn main() -> i32 {
  limit := ((0 as usize) - 1) / 2
  next := doubled_capacity(limit)
  if next <= limit {
    ret 1
  }
  _ := doubled_capacity(limit + 1)
  ret 0
}
