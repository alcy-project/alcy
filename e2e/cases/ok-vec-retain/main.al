// `retain` keeps the accepted elements in order and drops the rest.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  v.push(3)
  v.push(4)
  v.push(5)
  v.retain((x: &i32) -> *x % 2 == 1)
  if v.len() != 3 { ret 1 }
  if *v.at(0).unwrap() != 1 { ret 2 }
  if *v.at(1).unwrap() != 3 { ret 3 }
  if *v.at(2).unwrap() != 5 { ret 4 }
  mut w := Vec::<String>::new()
  w.push(format("a"))
  w.push(format("bb"))
  w.push(format("ccc"))
  w.retain((s: &String) -> s.len() > 1)
  if w.len() != 2 { ret 5 }
  if w.at(0).unwrap().as_str() != "bb" { ret 6 }
  if w.at(1).unwrap().as_str() != "ccc" { ret 7 }
  ret 0
}
