// A frame that reaches the stack reserve's end is a panic, not a
// corruption. The shadow stack and the heap share the memory above the
// data, so a deep enough recursion used to land on a live buffer and
// keep running with its bytes replaced. The first half pins that
// ordinary recursion under the limit still reads its heap right; the
// second asks for the panic. The check codes stay away from 1 so a
// failure here cannot be mistaken for the panic's own exit.
fn down(n: u64) -> u64 {
  if n == 0 {
    ret 0
  }
  ret down(n - 1) + 1
}

fn main() -> i32 {
  mut v := Vec::<u64>::with_capacity(4)
  v.push(111)
  v.push(222)
  r := down(300)
  if r != 300 {
    ret 40
  }
  match v.at(0) {
    Option::Some(x) => {
      if *x != 111 {
        ret 41
      }
    }
    Option::None => {
      ret 42
    }
  }
  _ := down(100000)
  ret 43
}
