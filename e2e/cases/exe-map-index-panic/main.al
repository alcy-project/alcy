// A missing key in `m[key]` names the map, not the `Option` the
// lookup handed back.
fn main() -> i32 {
  m := HashMap::<String, i32>::new()
  ret m[String::from("missing")]
}
