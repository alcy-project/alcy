// `<`, `<=`, `>`, and `>=` over a nominal type reach `PartialOrd`'s
// `partial_cmp`, and a `None` makes all four false. `Ord` narrows the
// same question to a total order for the types where every pair
// compares.

struct Temp {
  c: i32,
}

impl Temp {
  // Ordering by magnitude, so the comparison is not the field's own.
  fn magnitude(self: &Self) -> i32 {
    if self.c < 0 {
      ret -self.c
    }
    ret self.c
  }
}

impl PartialOrd for Temp {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    a := self.magnitude()
    b := other.magnitude()
    if a < b {
      ret Option::Some(Ordering::Less)
    }
    if a > b {
      ret Option::Some(Ordering::Greater)
    }
    ret Option::Some(Ordering::Equal)
  }
}

impl Ord for Temp {
  fn cmp(self: &Self, other: &Self) -> Ordering {
    a := self.magnitude()
    b := other.magnitude()
    if a < b {
      ret Ordering::Less
    }
    if a > b {
      ret Ordering::Greater
    }
    ret Ordering::Equal
  }
}

// A partial order only: a NaN has no place among the values, so every
// comparison against it is false.
struct Mix {
  v: f64,
}

impl PartialOrd for Mix {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    if self.v < other.v {
      ret Option::Some(Ordering::Less)
    }
    if self.v > other.v {
      ret Option::Some(Ordering::Greater)
    }
    if self.v == other.v {
      ret Option::Some(Ordering::Equal)
    }
    ret Option::None
  }
}

fn main() -> i32 {
  a := Temp { c: -5 }
  b := Temp { c: 3 }
  println("{} {} {} {}", a < b, a > b, a <= b, a >= b)
  c := Temp { c: 5 }
  println("{} {} {} {}", a < c, a <= c, a > c, a >= c)
  n := 0.0
  x := Mix { v: n / n }
  y := Mix { v: 1.0 }
  println("{} {} {} {}", x < y, x > y, x <= y, x >= y)
  z := Mix { v: 2.0 }
  println("{} {} {} {}", y < z, y <= z, y > z, y >= z)
  ret 0
}
