// `for pat in head { body }` iterates whatever `head.into_iter()`
// returns, as long as that cursor implements core's `Iterator`. The
// three pieces here are ordinary language code: an inherent `into_iter`,
// a cursor implementing the spec, and a `for` over it.
struct Countdown {
  remaining: i32,
}

impl Countdown {
  fn into_iter(self: Self) -> CountdownIter {
    ret CountdownIter { remaining: self.remaining }
  }
}

struct CountdownIter {
  remaining: i32,
}

impl Iterator<i32> for CountdownIter {
  fn next(mut self: &mut Self) -> Option<i32> {
    if self.remaining <= 0 {
      ret Option::None
    }
    v := self.remaining
    self.remaining = self.remaining - 1
    ret Option::Some(v)
  }
}

struct Pair {
  left: i32,
  right: i32,
}

struct Pairs {
  unused: i32,
}

impl Pairs {
  fn into_iter(self: Self) -> PairIter {
    ret PairIter { index: 0 }
  }
}

struct PairIter {
  index: i32,
}

impl Iterator<Pair> for PairIter {
  fn next(mut self: &mut Self) -> Option<Pair> {
    if self.index >= 3 {
      ret Option::None
    }
    i := self.index
    self.index = self.index + 1
    ret Option::Some(Pair { left: i, right: i * i })
  }
}

fn sum_to(limit: i32) -> i32 {
  c := Countdown { remaining: limit }
  mut total := 0
  for v in c {
    total = total + v
  }
  ret total
}

fn main() -> i32 {
  // A `for` asks the head for a cursor once and yields every item.
  c := Countdown { remaining: 4 }
  mut total := 0
  for v in c {
    total = total + v
  }
  if total != 10 { ret 1 }

  // `continue` and `break` act on the loop the rule generates.
  six := Countdown { remaining: 6 }
  mut odd_sum := 0
  for v in six {
    if v % 2 == 0 { continue }
    if v < 2 { break }
    odd_sum = odd_sum + v
  }
  if odd_sum != 8 { ret 2 }

  // Nested loops keep separate cursors, even though both desugar to a
  // binding with the same spelling.
  three := Countdown { remaining: 3 }
  mut grid := 0
  for a in three {
    for b in three {
      grid = grid + a * b
    }
  }
  if grid != 36 { ret 3 }

  // The pattern destructures the item; `_` skips a field.
  pairs := Pairs { unused: 0 }
  mut squares := 0
  for Pair { left: _, right } in pairs {
    squares = squares + right
  }
  if squares != 5 { ret 4 }

  // A wildcard pattern runs the body once per item; parentheses let a
  // struct literal stand as the head.
  mut seen := 0
  for _ in (Countdown { remaining: 7 }) {
    seen = seen + 1
  }
  if seen != 7 { ret 5 }
  if sum_to(3) != 6 { ret 6 }
  ret 0
}
