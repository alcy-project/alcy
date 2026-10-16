// Or-patterns distribute through a tuple, struct, or variant payload,
// and a variant payload may itself be tested.

enum E {
  A,
  B,
  C,
}

struct P {
  a: i32,
  b: i32,
}

fn pair(p: (E, i32)) -> i32 {
  ret match p {
    (E::A | E::B, z) => z,
    (E::C, z) => z + 100,
  }
}

fn record(p: P) -> i32 {
  ret match p {
    P { a: 1 | 2, b: x } => x,
    P { a: 3, b: x } => x + 100,
    P { a: _, b: x } => x + 200,
  }
}

fn payload(o: Option<i32>) -> i32 {
  ret match o {
    Option::Some(1 | 2) => 10,
    Option::Some(x) => x,
    Option::None => 0 - 1,
  }
}

fn nested(o: Option<Option<i32>>) -> i32 {
  ret match o {
    Option::Some(Option::Some(x)) => x,
    Option::Some(Option::None) => 0 - 1,
    Option::None => 0 - 2,
  }
}

fn enumopt(o: Option<E>) -> i32 {
  ret match o {
    Option::Some(E::A | E::C) => 1,
    Option::Some(E::B) => 2,
    Option::None => 3,
  }
}

fn main() -> i32 {
  if pair((E::A, 5)) != 5 {
    ret 1
  }
  if pair((E::B, 5)) != 5 {
    ret 2
  }
  if pair((E::C, 5)) != 105 {
    ret 3
  }
  if record((P { a: 1, b: 7 })) != 7 {
    ret 4
  }
  if record((P { a: 2, b: 7 })) != 7 {
    ret 5
  }
  if record((P { a: 3, b: 7 })) != 107 {
    ret 6
  }
  if record((P { a: 9, b: 7 })) != 207 {
    ret 7
  }
  if payload(Option::Some(1i32)) != 10 {
    ret 8
  }
  if payload(Option::Some(2i32)) != 10 {
    ret 9
  }
  if payload(Option::Some(5i32)) != 5 {
    ret 10
  }
  if payload(Option::None) != 0 - 1 {
    ret 11
  }
  if nested(Option::Some(Option::Some(5i32))) != 5 {
    ret 12
  }
  if nested(Option::Some(Option::None)) != 0 - 1 {
    ret 13
  }
  if nested(Option::None) != 0 - 2 {
    ret 14
  }
  if enumopt(Option::Some(E::A)) != 1 {
    ret 15
  }
  if enumopt(Option::Some(E::C)) != 1 {
    ret 16
  }
  if enumopt(Option::Some(E::B)) != 2 {
    ret 17
  }
  if enumopt(Option::None) != 3 {
    ret 18
  }
  ret 0
}
