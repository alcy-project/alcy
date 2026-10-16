// A name interner: one owner of the names, ids handed out by position.
struct Interner {
  names: Vec<String>,
}

impl Interner {
  fn new() -> Interner {
    ret Interner { names: Vec::<String>::new() }
  }

  fn intern(mut self: &mut Self, name: str) -> usize {
    mut i := 0 as usize
    while i < self.names.len() {
      if self.names.at(i).unwrap().as_str() == name {
        ret i
      }
      i = i + 1
    }
    self.names.push(String::from(name))
    ret self.names.len() - 1
  }

  fn name_of(self: &Self, id: usize) -> str {
    ret self.names.at(id).unwrap().as_str()
  }
}

fn main() -> i32 {
  mut pool := Interner::new()
  a := pool.intern("x")
  b := pool.intern("y")
  c := pool.intern("x")
  if a != 0 {
    ret 1
  }
  if b != 1 {
    ret 2
  }
  if c != 0 {
    ret 3
  }
  if pool.name_of(a) != "x" {
    ret 4
  }
  if pool.name_of(b) != "y" {
    ret 5
  }
  ret 0
}
