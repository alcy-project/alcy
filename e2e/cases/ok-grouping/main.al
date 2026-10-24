// Group words by their first byte, the shape a keyword table starts
// from.
fn main() -> i32 {
  mut groups := Map::<Vec<String>>::new()
  mut words := Vec::<String>::new()
  words.push(String::from("apple"))
  words.push(String::from("avocado"))
  words.push(String::from("berry"))
  mut i := 0 as usize
  while i < words.len() {
    w := words.at(i).or_panic_with_message("expected a value")
    key := w.byte_slice(0, 1)
    list := groups.get_or_insert_with(key, () -> Vec::<String>::new())
    list.push(String::from(w.as_str()))
    i = i + 1
  }
  if groups.len() != 2 {
    ret 1
  }
  match groups.get("a") {
    Option::Some(list) => {
      if list.len() != 2 {
        ret 2
      }
    }
    Option::None => {
      ret 3
    }
  }
  ret 0
}
