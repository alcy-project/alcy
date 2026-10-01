// A cursor can yield tuples: the spec argument, the substituted
// return type, and the second spelling inside the body must all name
// one instantiation, and the `for` pattern destructures the item.
struct PairIter {
  index: i32,
}

impl PairIter {
  fn into_iter(self: Self) -> PairIter {
    ret self
  }
}

impl Iterator<(i32, i32)> for PairIter {
  fn next(mut self: &mut Self) -> Option<(i32, i32)> {
    if self.index >= 3 {
      ret Option::None
    }
    i := self.index
    self.index = self.index + 1
    ret Option::Some((i, i * i))
  }
}

fn main() -> i32 {
  mut squares := 0
  mut count := 0
  for (i, sq) in (PairIter { index: 0 }) {
    squares = squares + sq
    count = count + i
  }
  if squares != 5 { ret 1 }
  if count != 3 { ret 2 }
  // The cursor stays spent through the same instantiation.
  mut it := PairIter { index: 0 }
  mut n := 0
  loop {
    match it.next() {
      Option::Some((_, _)) => {
        n = n + 1
      }
      Option::None => break,
    }
  }
  if n != 3 { ret 3 }
  if it.next().is_some() { ret 4 }
  ret 0
}
