// A shared receiver already hands out borrows of its fields, and the
// place stays readable afterwards. What the borrow checker cannot model
// is a place whose type is itself a reference; see docs/adr/0012-reborrow-on-reference-read.md.
struct Cell { n: i32, tag: i32 }

impl Cell {
  fn get(self: &Self) -> &i32 {
    ret &self.n
  }
  fn opt(self: &Self, i: i32) -> Option<&i32> {
    if i != 0 {
      ret Option::None
    }
    ret Option::Some(&self.n)
  }
}

fn main() -> i32 {
  c := Cell { n: 5, tag: 1 }
  p := c.get()
  if *p != 5 {
    ret 1
  }
  // Reborrow through a shared receiver, twice.
  q := c.get()
  if *q != 5 {
    ret 2
  }
  o := c.opt(0)
  if o.is_none() {
    ret 3
  }
  if *o.unwrap() != 5 {
    ret 4
  }
  if c.opt(1).is_some() {
    ret 5
  }
  // Read the field again after the borrows.
  if c.n != 5 {
    ret 6
  }
  ret 0
}
