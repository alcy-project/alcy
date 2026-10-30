// The token cursor over the standard input (ADR-0129): a count, that
// many integers, and the sum back out. The scanner is `io`'s implicit
// surface, so no `use` names it.

fn main() -> i32 {
  mut sc := Scanner::stdin()
  n := sc.next::<usize>().or_panic_with_message("n")
  xs := sc.next_vec::<i64>(n).or_panic_with_message("xs")
  mut sum := 0i64
  mut i := 0 as usize
  while i < n {
    match xs.at(i) {
      Option::Some(x) => {
        sum = sum + *x
      }
      Option::None => {
        ret 1
      }
    }
    i = i + 1
  }
  print("{}\n", sum)
  ret 0
}
