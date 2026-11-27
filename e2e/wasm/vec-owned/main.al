// A container whose element owns storage: pop, remove, truncate, and
// clear reach the element's destructor through a reference to it, and a
// nested container's destructor reaches its own elements the same way.
// The reference and the aggregate it names are one address word, so the
// cast between them relabels rather than converts.
fn measure(s: String) -> usize {
  ret s.len_bytes()
}

fn inner_len(v: Vec<i32>) -> usize {
  ret v.len()
}

fn main() -> i32 {
  mut v := Vec::<String>::new()
  v.push(String::from("alpha"))
  v.push(String::from("beta"))
  v.push(String::from("gamma"))
  if v.len() != 3 {
    ret 1
  }
  if measure(v.pop().or_panic("expected a value")) != 5usize {
    ret 2
  }
  if v.len() != 2 {
    ret 3
  }
  if measure(v.remove(0)) != 5usize {
    ret 4
  }
  if v.len() != 1 {
    ret 5
  }
  v.push(String::from("delta"))
  v.push(String::from("epsilon"))
  v.truncate(1)
  if v.len() != 1 {
    ret 6
  }
  if v.at(0).or_panic("expected a value").len_bytes() != 4usize {
    ret 7
  }
  if !v.at(0).or_panic("expected a value").starts_with("beta") {
    ret 8
  }
  v.clear()
  if !v.is_empty() {
    ret 9
  }
  v.push(String::from("zeta"))
  mut nested := Vec::<Vec<i32>>::new()
  mut inner := Vec::<i32>::new()
  inner.push(7i32)
  inner.push(8i32)
  nested.push(inner)
  mut other := Vec::<i32>::new()
  other.push(9i32)
  nested.push(other)
  if nested.len() != 2 {
    ret 10
  }
  if inner_len(nested.pop().or_panic("expected a value")) != 1 {
    ret 11
  }
  nested.clear()
  if v.at(0).or_panic("expected a value").len_bytes() != 4usize {
    ret 12
  }
  println("ok")
  ret 0
}
