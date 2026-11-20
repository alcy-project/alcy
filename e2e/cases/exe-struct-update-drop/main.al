// A move-update ends the fields it overwrites in place and moves the
// rest, so a non-Copy base is spent exactly once: the overwritten field
// does not leak and the surviving ones read as owned.
struct S {
  a: String,
  b: String,
}

fn from_param(s: S) -> i32 {
  t := S { a: String::from("x"), ..s }
  ret t.a.len_bytes() as i32 + t.b.len_bytes() as i32
}

fn make() -> S {
  ret S { a: String::from("aa"), b: String::from("bbb") }
}

fn main() -> i32 {
  s := S { a: String::from("aa"), b: String::from("bbb") }
  t := S { a: String::from("x"), ..s }
  if t.a.len_bytes() != 1 || t.b.len_bytes() != 3 {
    ret 1
  }
  arg := S { a: String::from("aa"), b: String::from("bbb") }
  if from_param(arg) != 4 {
    ret 2
  }
  u := S { a: String::from("y"), ..make() }
  if u.b.len_bytes() != 3 {
    ret 3
  }
  ret 0
}
