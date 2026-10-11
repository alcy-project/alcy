// The MapIter adapter: a function-typed field is an ordinary generic
// argument now that the mangle encodes function signatures, and the
// adapter's next calls it. Distinct element types instantiate apart.

fn inc(x: i32) -> i32 { ret x + 1 }

fn twice(x: i32) -> i64 { ret (x * 2) as i64 }

fn main() -> i32 {
  r := 1..=3
  mut m: MapIter<RangeIter<i32>, i32, i32> := MapIter { iter: r.into_iter(), f: inc }
  mut total := 0
  loop {
    match m.next() {
      Option::Some(v) => {
        total = total + v
      }
      Option::None => break,
    }
  }
  if total != 9 {
    ret 1
  }
  if m.next().is_some() {
    ret 2
  }

  s := 1..=3
  mut n: MapIter<RangeIter<i32>, i32, i64> := MapIter { iter: s.into_iter(), f: twice }
  mut sum := 0
  loop {
    match n.next() {
      Option::Some(v) => {
        sum = sum + v as i32
      }
      Option::None => break,
    }
  }
  if sum != 12 {
    ret 3
  }
  ret 0
}
