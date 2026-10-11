// The remaining iterator adapters: filter pulls while a predicate
// fails, enumerate pairs values with positions, take bounds the pulls,
// and skip drops a prefix; fold consumes an iterator into one value.

fn add(a: i32, b: i32) -> i32 { ret a + b }

fn even(x: i32) -> bool { ret x % 2 == 0 }

fn main() -> i32 {
  r := 1..=6
  mut f: FilterIter<RangeIter<i32>, i32> := FilterIter { iter: r.into_iter(), f: even }
  mut evens := 0
  loop {
    match f.next() {
      Option::Some(v) => {
        evens = evens + v
      }
      Option::None => break,
    }
  }
  if evens != 12 {
    ret 1
  }

  s := 10..=12
  mut e: EnumerateIter<RangeIter<i32>, (usize, i32)> := EnumerateIter { iter: s.into_iter(), index: 0 }
  mut positions := 0
  loop {
    match e.next() {
      Option::Some(p) => {
        positions = positions + (p.0 as i32) * 100 + p.1
      }
      Option::None => break,
    }
  }
  if positions != 333 {
    ret 2
  }

  t := 1..=10
  mut tk: TakeIter<RangeIter<i32>, i32> := TakeIter { iter: t.into_iter(), remaining: 3 }
  mut taken := 0
  loop {
    match tk.next() {
      Option::Some(_) => {
        taken = taken + 1
      }
      Option::None => break,
    }
  }
  if taken != 3 {
    ret 3
  }

  u := 1..=5
  mut sk: SkipIter<RangeIter<i32>, i32> := SkipIter { iter: u.into_iter(), remaining: 2, started: false }
  mut first := 0
  match sk.next() {
    Option::Some(v) => {
      first = v
    }
    Option::None => {}
  }
  if first != 3 {
    ret 4
  }

  w := 1..=4
  total := fold::<RangeIter<i32>, i32, i32>(w.into_iter(), 0, add)
  if total != 10 {
    ret 5
  }
  ret 0
}
