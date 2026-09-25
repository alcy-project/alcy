// Generic structs intern per instantiation, and their methods
// specialize per instantiation like generic enums do.
struct Pair<A, B> { first: A, second: B }

impl<A, B> Pair<A, B> {
  fn first_of(self: Self) -> A {
    ret self.first
  }
  fn second_of(self: Self) -> B {
    ret self.second
  }
  fn with_first(self: Self, v: A) -> Pair<A, B> {
    ret Pair { first: v, second: self.second }
  }
}

fn sum_pair(p: Pair<i32, i32>) -> i32 {
  ret p.first_of() + p.second_of()
}

fn same(a: str, b: str) -> bool {
  ret str_len(a) == str_len(b) && str_byte(a, 0) == str_byte(b, 0)
}

fn main() -> i32 {
  mut bad := 0
  p: Pair<i32, str> := Pair { first: 1i32, second: "hi" }
  q2: Pair<i32, i32> := Pair { first: 20i32, second: 22i32 }
  if p.first_of() != 1 {
    bad = 1
  }
  if str_len(p.second_of()) != 2 {
    bad = 2
  }
  if sum_pair(q2) != 42 {
    bad = 3
  }
  // Same generic type, different instantiation: distinct receivers.
  inner: Pair<i32, i32> := Pair { first: 1i32, second: 2i32 }
  n: Pair<Pair<i32, i32>, bool> := Pair { first: inner, second: true }
  if n.first_of().first_of() != 1 {
    bad = 4
  }
  if n.second_of() != true {
    bad = 5
  }
  q := p.with_first(9i32)
  if q.first_of() != 9 {
    bad = 6
  }
  if !same(q.second_of(), "hi") {
    bad = 7
  }
  ret bad
}
