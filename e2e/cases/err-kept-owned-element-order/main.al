// An element that owns its buffer carries a borrow through the field
// it allocates into, so keeping it in a container declared before the
// buffer it was built from is refused: a callee returning that field
// could hold the loan, and the extent rule cannot see that this one
// copies instead (ADR-0135).

fn main() -> i32 {
  mut names := Vec::<String>::new()
  key := String::from("ab")
  names.push(String::from(key.as_str()))
  println("{}", names[0].as_str())
  ret 0
}
