// A failed file_size path returns early. Registers the backend borrows
// across the call must still hold the caller's values afterwards.
fn main() -> i32 {
  guard := 12345
  _ := read_file_str(&Path::from_str("no/such/file.al"))
  if guard != 12345 {
    ret 1
  }
  ret 0
}
