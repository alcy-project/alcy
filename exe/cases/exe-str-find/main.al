fn main() -> i32 {
  s := format("hello world")
  if !s.contains("lo w") {
    ret 1
  }
  if s.contains("xyz") {
    ret 2
  }
  if s.find("world").unwrap() != 6 {
    ret 3
  }
  if s.find("world") != Option::Some(6 as usize) {
    ret 4
  }
  if s.find("xyz").is_some() {
    ret 5
  }
  if s.find("").unwrap() != 0 {
    ret 6
  }
  if !"abc".contains("abc") {
    ret 7
  }
  if "abc".contains("abcd") {
    ret 8
  }
  if "banana".find("na").unwrap() != 2 {
    ret 9
  }
  if !s.starts_with("hello") {
    ret 10
  }
  if !s.ends_with("world") {
    ret 11
  }
  ret 0
}
