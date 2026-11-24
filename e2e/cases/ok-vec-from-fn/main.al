// `from_fn` builds one element per index, so a value that cannot be
// copied can still fill a vector.

fn main() -> i32 {
  squares := Vec::<i32>::from_fn(5, (i: usize) -> (i * i) as i32)
  if squares.len() != 5 {
    ret 1
  }
  if squares[0] != 0 {
    ret 2
  }
  if squares[4] != 16 {
    ret 3
  }
  words := Vec::<String>::from_fn(3, (i: usize) -> format("v{}", i))
  if words.len() != 3 {
    ret 4
  }
  if words[0].as_str() != "v0" {
    ret 5
  }
  if words[2].as_str() != "v2" {
    ret 6
  }
  empty := Vec::<i32>::from_fn(0, (i: usize) -> 7)
  if !empty.is_empty() {
    ret 7
  }
  ret 0
}
