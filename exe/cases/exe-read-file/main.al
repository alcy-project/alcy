// `String::read_file` reads a whole file; a missing path is `None`.
fn main() -> i32 {
  match String::read_file("main.al") {
    Option::Some(text) => {
      if text.len() == 0 { ret 1 }
      if !text.as_str().contains("read_file") { ret 2 }
    }
    Option::None => {
      ret 3
    }
  }
  if String::read_file("/nonexistent/alcy-no-such-file").is_some() { ret 4 }
  ret 0
}
