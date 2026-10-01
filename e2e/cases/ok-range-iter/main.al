// Ranges iterate through their cursor: `for` unifies the item with
// the pattern, and a `Copy` head iterates twice.
fn sum_up_to(n: i32) -> i32 {
  mut total := 0
  for i in 0..<n {
    total = total + i
  }
  ret total
}

fn main() -> i32 {
  if sum_up_to(5) != 10 { ret 1 }
  mut inc := 0
  for i in 1..=4 {
    inc = inc + i
  }
  if inc != 10 { ret 2 }
  r := 0..<3
  mut twice := 0
  for i in r {
    twice = twice + i
  }
  for i in r {
    twice = twice + i
  }
  if twice != 6 { ret 3 }
  // A stepped head yields every stride-th value from the start.
  mut stepped := 0
  for i in (0..<10).step_by(3) {
    stepped = stepped + i
  }
  if stepped != 18 { ret 4 }
  ret 0
}
