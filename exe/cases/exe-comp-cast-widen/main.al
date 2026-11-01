// A cast widens a negative value by its own sign, and comp evaluation
// reads the same bits the run time does.
fn narrow(comp x: i32) -> i64 {
  ret (x as i8) as i64
}

const K: i64 = narrow(-84)

fn main() -> i32 {
  println("A {} {}", -81 as i64, comp { -81 as i64 })
  println("B {} {}", (-84 as i8) as i32, comp { (-84 as i8) as i32 })
  println("C {} {}", (0 - 5) as i64, comp { (0 - 5) as i64 })
  println("D {} {}", (-1 as i16) as u32, comp { (-1 as i16) as u32 })
  println("E {} {}", (200 as u8) as i8, comp { (200 as u8) as i8 })
  println("F {} {}", (-1 as i64) as i8, comp { (-1 as i64) as i8 })
  println("G {}", K)
  ret 0
}
