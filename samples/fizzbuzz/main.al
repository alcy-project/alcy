// FizzBuzz, the exercise every programmer is given: count to 20 and
// replace the multiples of three and five with words, both together
// where they coincide.
fn main() -> i32 {
  mut n := 1
  while n <= 20 {
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
    n = n + 1
  }
  ret 0
}
