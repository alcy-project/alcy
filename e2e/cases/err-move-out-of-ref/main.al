// A value-position `*` copies the referent, which would duplicate the
// ownership the place the reference was taken from still holds. The
// read of an owning, non-Copy value out of a shared reference is
// refused.
fn steal(r: &String) -> String {
  w := *r
  ret w
}

fn main() {
  mut t := String::new()
  t.push_str("hi")
  _ := steal(&t)
}
