// A generic map over a vector with a closure, the shape a pass uses.
fn map_all<T, U>(items: &Vec<T>, f: (&T) -> U) -> Vec<U> {
  mut out := Vec::<U>::new()
  mut i := 0 as usize
  while i < items.len() {
    out.push(f(items.at(i).unwrap()))
    i = i + 1
  }
  ret out
}

fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  doubled := map_all::<i32, i32>(&v, (x: &i32) -> *x * 2)
  if doubled.len() != 2 {
    ret 1
  }
  if *doubled.at(0).unwrap() != 2 {
    ret 2
  }
  if *doubled.at(1).unwrap() != 4 {
    ret 3
  }
  labels := map_all::<i32, String>(&v, (x: &i32) -> format("n{}", *x))
  if labels.at(0).unwrap().as_str() != "n1" {
    ret 4
  }
  ret 0
}
