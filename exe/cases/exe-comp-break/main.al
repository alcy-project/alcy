// A `break` or `continue` confined to a loop inside a comp block
// behaves normally (docs/spec/comp.md): the engine runs the loop and
// the block's value stays comp-known. Each ends the block's `if` or
// `match` arm and reaches the loop, not the boundary.
comp fn count(comp n: i32) -> i32 {
  ret comp {
    mut total := 0
    mut i := 0
    while i < n {
      i = i + 1
      if i == 2 {
        continue
      }
      if i > 5 {
        break
      }
      total = total + i
    }
    total
  }
}

// A `match` arm carries the change out the same way an `if` arm does.
comp fn pick(comp n: i32) -> i32 {
  ret comp {
    mut total := 0
    mut i := 0
    while i < n {
      i = i + 1
      match i {
        2 => { continue }
        7 => { break }
        _ => { total = total + i }
      }
    }
    total
  }
}

fn main() -> i32 {
  // 1..=5 without 2: 1 + 3 + 4 + 5 = 13.
  const ten := count(10)
  if ten != 13 {
    ret 1
  }
  const one := count(1)
  if one != 1 {
    ret 2
  }
  const zero := count(0)
  if zero != 0 {
    ret 3
  }
  // 1..=6 without 2: 1 + 3 + 4 + 5 + 6 = 19.
  const pten := pick(10)
  if pten != 19 {
    ret 4
  }
  ret 0
}
