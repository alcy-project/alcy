// An argument that names no instance leaves nothing to read the slot
// from: the call names the turbofish it needs.
fn first_of<T>(items: &Vec<T>) -> Option<&T> {
  ret items.at(0)
}

fn main() -> i32 {
  x := 5
  match first_of(&x) {
    Option::Some(v) => {
      ret 0
    }
    Option::None => {
      ret 1
    }
  }
}
