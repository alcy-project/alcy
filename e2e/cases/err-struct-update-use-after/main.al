// The update spends its base, so a later use is a moved-value use.
struct S {
  a: String,
  b: String,
}

fn main() -> i32 {
  s := S { a: String::from("aa"), b: String::from("bbb") }
  t := S { a: String::from("x"), ..s }
  ret s.b.len() as i32 + t.b.len() as i32
}
