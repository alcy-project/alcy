// A tuple or struct pattern tests a refutable field in place: a
// literal, a unit variant, or a variant with an irrefutable payload.

enum E { A, B, C }

struct P { flag: bool, n: i32 }

fn pair(p: (E, i32)) -> i32 {
  ret match p {
    (E::A, 1) => 10,
    (E::A, z) => z,
    (E::B, z) => z + 100,
    (E::C, z) => z + 200,
  }
}

fn record(p: P) -> i32 {
  ret match p {
    P { flag: true, n: 0 } => 1,
    P { flag: true, n: x } => x,
    P { flag: false, n: x } => x + 100,
  }
}

fn deep(p: ((E, i32), i32)) -> i32 {
  ret match p {
    ((E::A, 1), z) => 10 + z,
    ((E::A, x), z) => x + z,
    ((E::B, x), z) => x + z + 100,
  }
}

fn options(p: (Option<i32>, Option<i32>)) -> i32 {
  ret match p {
    (Option::Some(a), Option::None) => a,
    (Option::None, Option::Some(b)) => b + 100,
    (Option::Some(a), Option::Some(b)) => a + b,
    (Option::None, Option::None) => 0 - 1,
  }
}

fn main() -> i32 {
  if pair((E::A, 1)) != 10 { ret 1 }
  if pair((E::A, 5)) != 5 { ret 2 }
  if pair((E::B, 5)) != 105 { ret 3 }
  if pair((E::C, 5)) != 205 { ret 4 }
  if record((P { flag: true, n: 0 })) != 1 { ret 5 }
  if record((P { flag: true, n: 7 })) != 7 { ret 6 }
  if record((P { flag: false, n: 7 })) != 107 { ret 7 }
  if deep(((E::A, 1), 5)) != 15 { ret 8 }
  if deep(((E::A, 4), 5)) != 9 { ret 9 }
  if deep(((E::B, 4), 5)) != 109 { ret 10 }
  if options((Option::Some(5i32), Option::None)) != 5 { ret 11 }
  if options((Option::None, Option::Some(5i32))) != 105 { ret 12 }
  if options((Option::Some(2i32), Option::Some(3i32))) != 5 { ret 13 }
  if options((Option::None, Option::None)) != 0 - 1 { ret 14 }
  ret 0
}
