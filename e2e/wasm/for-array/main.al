// Consuming a by-value array walks it through the slice machinery, so
// the raw pointer a cursor keeps has to ride as an address word.
fn main() {
  a := [1, 2, 3]
  mut total := 0
  for x in a {
    total += x
  }
  println("{}", total)

  mut v := Vec::<i32>::new()
  v.push(4)
  v.push(5)
  mut sum := 0
  for y in v {
    sum += y
  }
  println("{}", sum)
}
