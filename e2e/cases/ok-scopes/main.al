// Nested scopes with shadowing: a stack of maps, looked up innermost
// first.
struct Scopes {
  frames: Vec<Map<i32>>,
}

impl Scopes {
  fn new() -> Scopes {
    ret Scopes { frames: Vec::<Map<i32>>::new() }
  }

  fn push(mut self: &mut Self) {
    self.frames.push(Map::<i32>::new())
  }

  fn pop(mut self: &mut Self) -> bool {
    if self.frames.is_empty() {
      ret false
    }
    _ := self.frames.pop()
    ret true
  }

  fn define(mut self: &mut Self, name: str, kind: i32) -> bool {
    if self.frames.is_empty() {
      ret false
    }
    _ := self.frames.last_mut().or_panic_with_message("expected a value").insert(name, kind)
    ret true
  }

  fn resolve(self: &Self, name: str) -> Option<i32> {
    mut i := self.frames.len()
    while i > 0 {
      i = i - 1
      match self.frames.at(i).or_panic_with_message("expected a value").get(name) {
        Option::Some(kind) => {
          ret Option::Some(*kind)
        }
        Option::None => {}
      }
    }
    ret Option::None
  }
}

fn main() -> i32 {
  mut s := Scopes::new()
  _ := s.push()
  if !s.define("x", 1) {
    ret 1
  }
  if s.resolve("x").or_panic_with_message("expected a value") != 1 {
    ret 2
  }
  _ := s.push()
  if !s.define("x", 2) {
    ret 3
  }
  if s.resolve("x").or_panic_with_message("expected a value") != 2 {
    ret 4
  }
  if !s.define("y", 5) {
    ret 5
  }
  if s.resolve("y").or_panic_with_message("expected a value") != 5 {
    ret 6
  }
  if !s.pop() {
    ret 7
  }
  if s.resolve("x").or_panic_with_message("expected a value") != 1 {
    ret 8
  }
  if s.resolve("y").is_some() {
    ret 9
  }
  ret 0
}
