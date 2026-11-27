// Words counted in a `HashMap`, then read back a key at a time.
// `words.iter()` yields a reference to each element, so `*w` is the
// `str` the map keys on; `keys()` yields a reference to the key.
fn main() -> i32 {
  words := ["the", "cat", "and", "the", "hat", "the", "cat"]
  mut counts := HashMap::<str, i32>::new()
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
    n := counts.get_at(key).or_panic("every key is present")
    println("{} {}", *key, *n)
    total = total + *n
  }
  println("total = {}", total)
  ret 0
}
