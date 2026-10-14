// FizzBuzz: a multiple of both three and five prints both words, so
// that branch comes first.
fn main() -> i32 {
  for n in 1..=20 {
    both := n % 15 == 0
    three := n % 3 == 0
    five := n % 5 == 0
    if both {
      println("FizzBuzz")
    } else if three {
      println("Fizz")
    } else if five {
      println("Buzz")
    } else {
      println("{}", n)
    }
  }
  ret 0
}
