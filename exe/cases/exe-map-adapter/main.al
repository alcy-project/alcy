// `Vec::map`'s result type comes from the closure return, so a call
// spells neither type argument.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  v.push(2i32)
  v.push(3i32)
  mapped := v.map((x: &i32) -> *x + 1)
  if mapped.len() != 3 { ret 1 }
  if *mapped.at(0).or_panic_with_message("the vector holds three elements") != 2 { ret 2 }
  if *mapped.at(2).or_panic_with_message("the vector holds three elements") != 4 { ret 3 }
  flags := v.map((x: &i32) -> *x > 1)
  if flags.len() != 3 { ret 4 }
  if !*flags.at(1).or_panic_with_message("the vector holds three elements") { ret 5 }
  doubled := v.map((x: &i32) -> *x * 2)
  sum := *doubled.at(0).or_panic_with_message("the vector holds three elements") + *doubled.at(1).or_panic_with_message("the vector holds three elements") + *doubled.at(2).or_panic_with_message("the vector holds three elements")
  if sum != 12 { ret 6 }
  ret 0
}
