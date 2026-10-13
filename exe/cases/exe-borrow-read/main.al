// Reading through a reference: a `Copy` field of a borrowed value, an
// element of a borrowed buffer, a method on either, and a `Copy`
// payload matched through a reference are reads, not moves, so the
// owner still ends the value exactly once. Writing through a view
// lands in the buffer's owner.

struct S {
  n: i32,
  name: String,
}

enum C {
  Pair(i32, i32),
  Zero,
}

fn first(v: &[i32]) -> i32 {
  ret v[0]
}

fn main() -> i32 {
  mut t := String::new()
  t.push_str("hi")
  s := S { n: 5, name: t }
  r := &s
  if r.n != 5 {
    ret 1
  }
  if (*r).n != 5 {
    ret 2
  }
  if r.name.len() != 2 {
    ret 3
  }
  if (*r).name.len() != 2 {
    ret 4
  }

  mut a := [1i32, 2i32]
  if first(&a) != 1 {
    ret 5
  }
  if a[1] != 2 {
    ret 6
  }
  a[1] = 3
  if a[1] != 3 {
    ret 7
  }

  mut v := Vec::<i32>::new()
  v.push(9)
  if v[0] != 9 {
    ret 8
  }
  v[0] = 4
  if v.as_slice()[0] != 4 {
    ret 9
  }

  c := C::Pair(7, 8)
  label := match *&c {
    C::Pair(n, _) => n,
    C::Zero => 0,
  }
  if label != 7 {
    ret 10
  }
  println("{} {} {} {}", r.n, r.name.len(), v[0], label)
  ret 0
}
