// A value of an enum type consumed on one arm and not the other is
// ended on the path that still owns it: the end reads the
// discriminant and each payload field through the place, so the
// ownership marker has to come before both (ADR-0062).

struct D {
  n: i32,
}

impl D {
  fn drop(self: D) {
    println("drop {}", self.n)
  }
}

enum Holder {
  One(D),
  Two(i32),
}

fn take(h: Holder) -> i32 {
  ret 1
}

fn takeopt(o: Option<D>) -> i32 {
  ret 2
}

fn main() -> i32 {
  mut i := 0
  while i < 3 {
    h := Holder::One(D { n: i })
    if i == 1 {
      _ := take(h)
    }
    i += 1
  }
  mut o := Option::<D>::Some(D { n: 7 })
  c := false
  if c {
    _ := takeopt(o)
  } else {
    println("kept")
  }
  ret 0
}
