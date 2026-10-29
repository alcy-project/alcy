// A type argument inside a generic instance infers from the argument.
fn first_of<T>(items: &Vec<T>) -> Option<&T> {
  ret items.at(0)
}

fn map_all<T, U>(items: &Vec<T>, f: (&T) -> U) -> Vec<U> {
  mut out := Vec::<U>::new()
  mut i := 0 as usize
  while i < items.len() {
    out.push(f(items.at(i).or_panic_with_message("the index is within the vec")))
    i = i + 1
  }
  ret out
}

fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  match first_of(&v) {
    Option::Some(x) => {
      if *x != 1 { ret 1 }
    }
    Option::None => {
      ret 2
    }
  }
  doubled := map_all(&v, (x: &i32) -> *x * 2)
  if *doubled.at(0).or_panic_with_message("the vector holds two elements") != 2 { ret 3 }
  if *doubled.at(1).or_panic_with_message("the vector holds two elements") != 4 { ret 4 }
  labels := map_all(&v, (x: &i32) -> format("n{}", *x))
  if labels.at(0).or_panic_with_message("the vector holds two elements").as_str() != "n1" { ret 5 }
  ret 0
}
