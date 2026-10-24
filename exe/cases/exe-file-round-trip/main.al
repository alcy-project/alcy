// A compiler reads a source file and writes its output back out.
fn main() -> i32 {
  path := "alcy-probe-io.txt"
  if !String::write_file(path, "hello\nworld") {
    ret 1
  }
  match String::read_file(path) {
    Option::Some(back) => {
      if back.as_str() != "hello\nworld" {
        ret 2
      }
      if back.len_bytes() != 11 {
        ret 3
      }
      if back.byte(5) != 0x0au8 {
        ret 4
      }
    }
    Option::None => {
      ret 5
    }
  }
  ret 0
}
