// A temporary that owns memory is not borrowed: the drop walk ends it
// at the statement's end, so a reference to it would outlive it.
fn main() -> i32 {
  s := String::new()
  ret s.eq(&String::new()) as i32
}
