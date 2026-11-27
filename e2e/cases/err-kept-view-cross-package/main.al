// The keep travels across the package boundary: the dependency's body
// stores the view, and the caller's argument escapes its block.

use dep::store::store;

fn main() -> i32 {
  mut holder := Vec::<(str, i32)>::new()
  {
    buf := String::from("abc")
    store(&mut holder, buf.as_str())
  }
  w := holder.at(0).or_panic("none")
  print("[{}]", w.0)
  ret 0
}
