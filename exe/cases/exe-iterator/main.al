// Iteration is explicit: a `loop` calling `next` until `None` is the
// shape the `for` rule will desugar to, and it works on any type that
// implements core's `Iterator`.
use self::cursors::Countdown;
use self::cursors::bytes;

fn main() -> i32 {
  mut total := 0
  mut c := Countdown::new(4)
  loop {
    match c.next() {
      Option::Some(v) => {
        total = total + v
      }
      Option::None => break,
    }
  }
  if total != 10 { ret 1 }
  if c.next().is_some() { ret 2 }

  mut sum := 0
  mut b := bytes([1u8, 2u8, 3u8])
  loop {
    match b.next() {
      Option::Some(v) => {
        sum = sum + v as i32
      }
      Option::None => break,
    }
  }
  if sum != 6 { ret 3 }
  ret 0
}
