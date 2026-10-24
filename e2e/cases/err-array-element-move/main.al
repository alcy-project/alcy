// An array ends its elements whole: a value with a destructor cannot
// be moved out from under one yet, because the array's glue would end
// what the move took a second time.
fn main() -> i32 {
  a := [String::new(), String::new()]
  w := a[0]
  ret w.len_bytes() as i32
}
