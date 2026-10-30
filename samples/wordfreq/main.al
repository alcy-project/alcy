// Words counted in a `Map`, then read back a key at a time.
// `words.iter()` yields a reference to each element, so `*w` is the
// `str` the map keys on; `keys()` yields the key itself.
fn main() -> i32 {
  words := ["the", "cat", "and", "the", "hat", "the", "cat"]
  mut counts := Map::<i32>::new()
  for w in words.iter() {
    mut next := 1
    if Option::Some(n) := counts.get(*w) {
      next = *n + 1
    }
    _ := counts.insert(*w, next)
  }
  println("distinct = {}", counts.len())

  mut total := 0i32
  for key in counts.keys() {
    n := counts.get(key).or_panic_with_message("every key is present")
    println("{} {}", key, *n)
    total = total + *n
  }
  println("total = {}", total)
  ret 0
}
