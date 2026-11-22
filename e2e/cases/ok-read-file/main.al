// `read_file_str` reads a whole file; a missing path is `None`.
fn main() -> i32 {
  match read_file_str(&Path::from_str("main.al")) {
    Option::Some(text) => {
      if text.len_bytes() == 0 {
        ret 1
      }
      if !text.as_str().contains("read_file_str") {
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
