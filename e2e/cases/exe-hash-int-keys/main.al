// Integer keys use the `Hash` spec directly: no string allocation, no
// borrowed view. The map owns the key it is handed, so a caller keeps
// using its own integers while the table grows.

fn main() -> i32 {
  mut counts := HashMap::<i64, i32>::new()
  mut seen := HashSet::<i64>::new()
  mut i := 0
  while i < 200 {
    k := (i % 7) as i64
    _ := counts.get_or_insert(k, 0)
    mut slot := counts.get_mut(k).or_panic("the key was just inserted")
    *slot = *slot + 1
    _ := seen.insert(k)
    i = i + 1
  }
  if counts.len() != 7 {
    ret 1
  }
  if seen.len() != 7 {
    ret 2
  }
  if counts[0i64] != 29 {
    ret 3
  }
  if counts[6i64] != 28 {
    ret 4
  }
  if !seen.contains(3i64) {
    ret 5
  }
  if seen.contains(9i64) {
    ret 6
  }
  if counts.remove(0i64).or_panic("the key was there") != 29 {
    ret 7
  }
  if counts.len() != 6 {
    ret 8
  }
  ret 0
}
