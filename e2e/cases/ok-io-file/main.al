// The io prelude exposes whole-file reads and writes.
fn main() -> i32 {
  if !write_file_str(&Path::from_str("t.txt"), "abc") {
    ret 1
  }
  match read_file_str(&Path::from_str("t.txt")) {
    Option::Some(text) => {
      if text.as_str() != "abc" {
        ret 2
      }
    }
    Option::None => {
      ret 3
    }
  }
  if read_file_str(&Path::from_str("/nonexistent/alcy-no-such-file")).is_some() {
    ret 4
  }
  ret 0
}
