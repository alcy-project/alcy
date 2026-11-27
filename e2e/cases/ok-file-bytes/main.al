// Byte file I/O writes bytes unchanged, including ones that are not
// well-formed UTF-8.

fn main() -> i32 {
  path := Path::from_str("alcy-probe-bytes.bin")
  data := [0x41u8, 0xffu8, 0x00u8, 0x42u8]
  if !write_file_bytes(&path, data.as_slice()) {
    ret 1
  }
  match read_file_bytes(&path) {
    Option::Some(back) => {
      if back.len() != 4 {
        ret 2
      }
      if *back.at(0).or_panic("first byte") != 0x41u8 {
        ret 3
      }
      if *back.at(1).or_panic("second byte") != 0xffu8 {
        ret 4
      }
      if *back.at(2).or_panic("third byte") != 0x00u8 {
        ret 5
      }
      if *back.at(3).or_panic("fourth byte") != 0x42u8 {
        ret 6
      }
    }
    Option::None => {
      ret 7
    }
  }
  if read_file_str(&path).is_some() {
    ret 8
  }
  ret 0
}
