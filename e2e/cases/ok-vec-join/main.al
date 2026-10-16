// `join` puts `sep` between string elements.
fn main() -> i32 {
  mut v := Vec::<String>::new()
  v.push(format("a"))
  v.push(format("b"))
  v.push(format("c"))
  r := join(&v, ", ")
  if r.as_str() != "a, b, c" {
    ret 1
  }
  mut e := Vec::<String>::new()
  if join(&e, "-").len() != 0 {
    ret 2
  }
  mut one := Vec::<String>::new()
  one.push(format("x"))
  if join(&one, "-").as_str() != "x" {
    ret 3
  }
  ret 0
}
