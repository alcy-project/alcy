// `write_file_str` writes a file that `read_file_str` reads back.
fn main() -> i32 {
  if !write_file_str(&Path::from_str("out.txt"), "hello file") {
    ret 1
  }
  match read_file_str(&Path::from_str("out.txt")) {
    Option::Some(text) => {
      if text.as_str() != "hello file" {
        ret 2
      }
    }
    Option::None => {
      ret 3
    }
  }
  if write_file_str(&Path::from_str("/nonexistent/dir/x"), "y") {
    ret 4
  }
  ret 0
}
