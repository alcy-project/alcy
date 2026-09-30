// A method on a generic field must borrow the field itself. This is a
// plain wrapper over `Vec<T>`: if `self.items.push` copied the field,
// the pushes would land in a temporary and `size` would stay zero.
struct Bag<T> {
  items: Vec<T>
}

impl<T> Bag<T> {
  fn add(self: &mut Self, x: T) {
    self.items.push(x)
  }

  fn size(self: &Self) -> usize {
    ret self.items.len()
  }

  fn take(self: &mut Self) -> Option<T> {
    ret self.items.pop()
  }
}

fn main() -> i32 {
  mut b: Bag<i32> := Bag { items: Vec::<i32>::new() }
  if b.size() != 0 {
    ret 1
  }
  b.add(7i32)
  b.add(9i32)
  if b.size() != 2 {
    ret 2
  }
  last := b.take()
  if last.is_none() {
    ret 3
  }
  if last.unwrap() != 9i32 {
    ret 4
  }
  if b.size() != 1 {
    ret 5
  }
  mut t: Bag<u8> := Bag { items: Vec::<u8>::new() }
  t.add(3 as u8)
  if t.size() != 1 {
    ret 6
  }
  if *t.items.at(0).unwrap() != 3 {
    ret 7
  }
  ret 0
}
