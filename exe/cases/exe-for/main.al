use self::cursors::Countdown;
use self::cursors::Wrapped;

fn main() -> i32 {
  // The loop yields every item, in order, and leaves the Copy head
  // untouched: `c` still stands at its first value afterwards.
  mut c := Countdown::new(4)
  mut total := 0
  for v in c {
    total = total + v
  }
  if total != 10 { ret 1 }
  match c.next() {
    Option::Some(v) => {
      if v != 4 { ret 2 }
    }
    Option::None => { ret 3 }
  }

  // An explicit call sees `Wrapped`'s inherent `next`...
  mut w := Wrapped::new(3)
  match w.next() {
    Option::Some(v) => {
      if v != -100 { ret 4 }
    }
    Option::None => { ret 5 }
  }
  // ...while `for` reaches the `Iterator` implementation.
  mut count := 0
  mut sum := 0
  for v in Wrapped::new(3) {
    count = count + 1
    sum = sum + v
  }
  if count != 3 { ret 6 }
  if sum != 6 { ret 7 }

  // `continue` and `break` act on the generated loop.
  mut evens := 0
  for v in Countdown::new(10) {
    if v % 2 != 0 { continue }
    if v < 4 { break }
    evens = evens + v
  }
  if evens != 28 { ret 8 }

  // Nested loops run independently.
  mut grid := 0
  for a in Countdown::new(3) {
    for b in Countdown::new(3) {
      grid = grid + a * b
    }
  }
  if grid != 36 { ret 9 }
  ret 0
}
