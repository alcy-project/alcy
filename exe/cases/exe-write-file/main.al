// `String::write_file` writes a file that `read_file` reads back.
fn main() -> i32 {
  if !String::write_file("out.txt", "hello file") {
    ret 1
  }
  match String::read_file("out.txt") {
    Option::Some(text) => {
      if text.as_str() != "hello file" {
        ret 2
      }
    }
    Option::None => {
      ret 3
    }
  }
  if String::write_file("/nonexistent/dir/x", "y") {
    ret 4
  }
  ret 0
}
