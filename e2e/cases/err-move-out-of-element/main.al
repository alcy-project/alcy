// Refusing a move out of an array's element ends lowering for that
// expression: the refused value carries an error type, so a later
// projection into it would report a missing field on top of the
// refusal instead of leaving the one diagnostic that names the gap.
struct Res {
  n: i32,
}

impl Res {
  pub fn drop(self: Res) {
    println("drop {}", self.n)
  }
}

fn main() -> i32 {
  arr := [Res { n: 1 }, Res { n: 2 }]
  a := arr[0]
  ret a.n
}
