// `retain` keeps the accepted elements in order and drops the rest.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  v.push(3)
  v.push(4)
  v.push(5)
  v.retain((x: &i32) -> *x % 2 == 1)
  if v.len() != 3 {
    ret 1
  }
  if *v.at(0).or_panic_with_message("expected a value") != 1 {
    ret 2
  }
  if *v.at(1).or_panic_with_message("expected a value") != 3 {
    ret 3
  }
  if *v.at(2).or_panic_with_message("expected a value") != 5 {
    ret 4
  }
  mut w := Vec::<String>::new()
  w.push(format("a"))
  w.push(format("bb"))
  w.push(format("ccc"))
  w.retain((s: &String) -> s.len_bytes() > 1)
  if w.len() != 2 {
    ret 5
  }
  if w.at(0).or_panic_with_message("expected a value").as_str() != "bb" {
    ret 6
  }
  if w.at(1).or_panic_with_message("expected a value").as_str() != "ccc" {
    ret 7
  }
  ret 0
}
