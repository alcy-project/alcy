// A type argument inside a generic instance infers from the argument.
fn first_of<T>(items: &Vec<T>) -> Option<&T> {
  ret items.at(0)
}

// Each slot binds from the argument that nests it.
fn pair_len<T, U>(a: &Vec<T>, b: &Vec<U>) -> usize {
  ret a.len() + b.len()
}

fn map_all<T, U>(items: &Vec<T>, f: (&T) -> U) -> Vec<U> {
  mut out := Vec::<U>::new()
  mut i := 0 as usize
  while i < items.len() {
    out.push(f(items.at(i).or_panic_with_message("index")))
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
      if *x != 1 {
        ret 1
      }
    }
    Option::None => {
      ret 2
    }
  }
  mut w := Vec::<String>::new()
  w.push(String::from("x"))
  if pair_len(&v, &w) != 3 {
    ret 3
  }
  // A closure argument pins the callee's parameter from its own
  // parameter's type and the return slot from its body.
  doubled := map_all(&v, (x: &i32) -> *x * 2)
  if *doubled.at(0).or_panic_with_message("index 0") != 2 {
    ret 4
  }
  labels := map_all(&v, (x: &i32) -> format("n{}", *x))
  if labels.at(0).or_panic_with_message("index 0").as_str() != "n1" {
    ret 5
  }
  ret 0
}
