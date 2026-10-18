// A run whose elements are aggregates: the run is still a view, so a
// method on it reads through the reference the index lowers to rather
// than an element place.
struct Pair {
  a: i64,
  b: i64,
}

fn main() -> i32 {
  mut v := Vec::<Pair>::new()
  v.push(Pair { a: 1i64, b: 11i64 })
  v.push(Pair { a: 2i64, b: 22i64 })
  v.push(Pair { a: 3i64, b: 33i64 })
  s := v.as_slice()
  if s[1..<3].len() != 2 { ret 1 }
  if s[1..<3].at(0).unwrap().a != 2i64 { ret 2 }
  if s[1..<3].at(1).unwrap().b != 33i64 { ret 3 }
  if s[..<2].at(1).unwrap().a != 2i64 { ret 4 }
  if s[1..].at(0).unwrap().a != 2i64 { ret 5 }
  if s[0..=1].at(1).unwrap().a != 2i64 { ret 6 }
  if s[1..<3].first().is_some() != true { ret 7 }
  if s[1..<3].last().unwrap().a != 3i64 { ret 8 }
  if s[1..<3].position((p: &Pair) -> p.a == 3i64).unwrap() != 1 { ret 9 }
  ret 0
}
