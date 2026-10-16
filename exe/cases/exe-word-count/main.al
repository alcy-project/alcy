// Word frequencies: a map keyed by a view, a cell grown in place.
fn main() -> i32 {
  mut counts := Map::<i32>::new()
  for word in "a b a c a".split_whitespace() {
    cell := counts.get_or_insert_with(word, () -> 0)
    *cell = *cell + 1
  }
  if counts.len() != 3 {
    ret 1
  }
  match counts.get("a") {
    Option::Some(n) => {
      if *n != 3 {
        ret 2
      }
    }
    Option::None => {
      ret 3
    }
  }
  ret 0
}
