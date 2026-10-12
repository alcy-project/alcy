// `Vec::sort` carries `T: Ord`, so a vector of a type without the
// total order is refused at the call.

struct Plain {
  n: i32,
}

fn main() {
  mut v: Vec<Plain> := Vec::<Plain>::new()
  v.push(Plain { n: 1 })
  v.push(Plain { n: 2 })
  v.sort()
}
