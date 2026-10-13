// Moving a non-`Copy` element out of a buffer by index: indexing names
// the element through a reference, and the buffer's owner still ends
// what that reference points at.
fn steal(v: Vec<String>) -> String {
  ret v[0]
}

fn main() {
  mut v := Vec::<String>::new()
  v.push(String::new())
  ret
}
