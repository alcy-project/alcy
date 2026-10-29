fn first_of<T>(items: &Vec<Option<T>>) -> Option<&T> {
  match items.at(0) {
    Option::Some(v) => {
      match v {
        Option::Some(inner) => {
          ret Option::Some(inner)
        }
        Option::None => {
          ret Option::None
        }
      }
    }
    Option::None => {
      ret Option::None
    }
  }
}

fn main() -> i32 {
  mut v := Vec::<Option<i32>>::new()
  v.push(Option::Some(9))
  match first_of(&v) {
    Option::Some(x) => {
      if *x != 9 { ret 1 }
    }
    Option::None => {
      ret 2
    }
  }
  ret 0
}
