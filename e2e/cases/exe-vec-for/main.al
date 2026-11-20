// `for` walks a vector through its consuming cursor.

fn main() -> i32 {
  mut v: Vec<i32> := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  v.push(3)
  mut total := 0
  for x in v {
    total = total + x
  }
  if total != 6 {
    ret 1
  }
  ret 0
}
