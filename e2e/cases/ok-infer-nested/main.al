// A type argument inside a generic instance infers from the argument.
fn first_of<T>(items: &Vec<T>) -> Option<&T> {
  ret items.at(0)
}

// Each slot binds from the argument that nests it.
fn pair_len<T, U>(a: &Vec<T>, b: &Vec<U>) -> usize {
  ret a.len() + b.len()
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
  ret 0
}
