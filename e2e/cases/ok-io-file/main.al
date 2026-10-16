// The io prelude exposes whole-file reads and writes.
fn main() -> i32 {
  if !write_file("t.txt", "abc") {
    ret 1
  }
  match read_file("t.txt") {
    Option::Some(text) => {
      if text.as_str() != "abc" {
        ret 2
      }
    }
    Option::None => {
      ret 3
    }
  }
  if read_file("/nonexistent/alcy-no-such-file").is_some() {
    ret 4
  }
  ret 0
}
