// A map of lists: non-Copy values behind `get_mut`, the shape a
// front end uses to group declarations by name.
fn main() -> i32 {
  mut buckets := Map::<Vec<i32>>::new()
  _ := buckets.insert("a", Vec::<i32>::new())
  match buckets.get_mut("a") {
    Option::Some(list) => {
      list.push(1)
      list.push(2)
    }
    Option::None => {
      ret 1
    }
  }
  match buckets.get("a") {
    Option::Some(list) => {
      if list.len() != 2 { ret 2 }
    }
    Option::None => {
      ret 3
    }
  }
  if buckets.len() != 1 { ret 4 }
  ret 0
}
