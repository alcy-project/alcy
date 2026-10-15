// A generic stack, used at two element types.
struct Stack<T> {
  items: Vec<T>,
}

impl<T> Stack<T> {
  fn new() -> Stack<T> {
    ret Stack::<T> { items: Vec::<T>::new() }
  }

  fn push(mut self: &mut Self, value: T) {
    self.items.push(value)
  }

  fn pop(mut self: &mut Self) -> Option<T> {
    ret self.items.pop()
  }

  fn len(self: &Self) -> usize {
    ret self.items.len()
  }
}

fn main() -> i32 {
  mut ints := Stack::<i32>::new()
  ints.push(1)
  ints.push(2)
  match ints.pop() {
    Option::Some(v) => {
      if v != 2 { ret 1 }
    }
    Option::None => {
      ret 2
    }
  }
  if ints.len() != 1 { ret 3 }
  mut names := Stack::<String>::new()
  names.push(String::from("x"))
  match names.pop() {
    Option::Some(v) => {
      if v.as_str() != "x" { ret 4 }
    }
    Option::None => {
      ret 5
    }
  }
  ret 0
}
