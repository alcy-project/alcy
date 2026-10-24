// The shifting operations move elements: insert and swap walk a run of
// them, retain compacts in place, reverse works from both ends, extend
// copies a second buffer. Each width has its own copy, so the case
// carries an eight-byte element, a one-byte one, and a sixteen-byte
// struct.
struct Pair {
  a: i64,
  b: i64,
}

fn main() -> i32 {
  mut v := Vec::<i64>::new()
  v.push(1i64)
  v.push(2i64)
  v.push(4i64)
  v.insert(2, 3i64)
  if v.len() != 4 {
    ret 1
  }
  if *v.at(2).or_panic_with_message("expected a value") != 3i64 {
    ret 2
  }
  v.insert(0, 0i64)
  if *v.at(0).or_panic_with_message("expected a value") != 0i64 {
    ret 3
  }
  if *v.at(4).or_panic_with_message("expected a value") != 4i64 {
    ret 4
  }
  v.insert(5, 5i64)
  if *v.at(5).or_panic_with_message("expected a value") != 5i64 {
    ret 5
  }
  v.swap(0, 5)
  if *v.at(0).or_panic_with_message("expected a value") != 5i64 {
    ret 6
  }
  if *v.at(5).or_panic_with_message("expected a value") != 0i64 {
    ret 7
  }
  v.reverse()
  if *v.at(0).or_panic_with_message("expected a value") != 0i64 {
    ret 8
  }
  if *v.at(5).or_panic_with_message("expected a value") != 5i64 {
    ret 9
  }
  v.retain((x: &i64) -> *x % 2i64 == 0i64)
  if v.len() != 3 {
    ret 10
  }
  if *v.at(0).or_panic_with_message("expected a value") != 0i64 {
    ret 11
  }
  if *v.at(1).or_panic_with_message("expected a value") != 4i64 {
    ret 12
  }
  if *v.at(2).or_panic_with_message("expected a value") != 2i64 {
    ret 13
  }
  mut w := Vec::<i64>::new()
  w.push(7i64)
  w.push(8i64)
  v.extend(w)
  if v.len() != 5 {
    ret 14
  }
  if *v.at(3).or_panic_with_message("expected a value") != 7i64 {
    ret 15
  }
  if *v.at(4).or_panic_with_message("expected a value") != 8i64 {
    ret 16
  }
  mut p := v.at_mut(2).or_panic_with_message("expected a value")
  *p = 22i64
  if *v.at(2).or_panic_with_message("expected a value") != 22i64 {
    ret 17
  }

  mut b := Vec::<u8>::new()
  b.push(1u8)
  b.push(2u8)
  b.push(3u8)
  b.insert(0, 9u8)
  if b.len() != 4 {
    ret 18
  }
  if *b.at(0).or_panic_with_message("expected a value") != 9u8 {
    ret 19
  }
  b.reverse()
  if *b.at(0).or_panic_with_message("expected a value") != 3u8 {
    ret 20
  }
  if *b.at(3).or_panic_with_message("expected a value") != 9u8 {
    ret 21
  }
  b.retain((x: &u8) -> *x < 9u8)
  if b.len() != 3 {
    ret 22
  }

  mut q := Vec::<Pair>::new()
  q.push(Pair { a: 1i64, b: 11i64 })
  q.push(Pair { a: 2i64, b: 22i64 })
  q.insert(1, Pair { a: 3i64, b: 33i64 })
  if q.len() != 3 {
    ret 23
  }
  if q.at(1).or_panic_with_message("expected a value").a != 3i64 {
    ret 24
  }
  if q.at(1).or_panic_with_message("expected a value").b != 33i64 {
    ret 25
  }
  if q.at(2).or_panic_with_message("expected a value").b != 22i64 {
    ret 26
  }
  q.reverse()
  if q.at(0).or_panic_with_message("expected a value").a != 2i64 {
    ret 27
  }
  if q.at(2).or_panic_with_message("expected a value").a != 1i64 {
    ret 28
  }
  q.retain((x: &Pair) -> x.a != 3i64)
  if q.len() != 2 {
    ret 29
  }
  if q.at(0).or_panic_with_message("expected a value").b != 22i64 {
    ret 30
  }
  ret 0
}
