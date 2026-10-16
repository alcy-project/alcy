// Ordered comparison reaches a string's `partial_cmp`.
fn main() -> i32 {
  if "a" >= "b" {
    ret 1
  }
  if !("a" < "b") {
    ret 2
  }
  if "ab" >= "b" {
    ret 3
  }
  if "abc" <= "ab" {
    ret 4
  }
  if !("abc" > "ab") {
    ret 5
  }
  mut names := Vec::<String>::new()
  names.push(format("b"))
  names.push(format("a"))
  names.push(format("ab"))
  names.sort()
  if names.at(0).unwrap().as_str() != "a" {
    ret 6
  }
  if names.at(1).unwrap().as_str() != "ab" {
    ret 7
  }
  if names.at(2).unwrap().as_str() != "b" {
    ret 8
  }
  ret 0
}
