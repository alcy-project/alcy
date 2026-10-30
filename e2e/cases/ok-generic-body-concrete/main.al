// A generic body's call on a concrete receiver instantiates under the
// definition-site key, and the row never lowers (ADR-0079): the
// program builds even though nothing instantiates `g`.
fn g<T>(x: T, v: &mut Vec<i32>) {
  v.push(1)
}

fn main() {}
