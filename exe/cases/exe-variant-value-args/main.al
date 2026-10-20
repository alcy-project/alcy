// A tuple variant's owner turbofish names the instance, including a
// value parameter the payload alone cannot pin down.
enum Vec2<T, N: usize> {
  Items([T; N]),
  Empty,
}

fn main() -> i32 {
  e := Vec2::<i32, 2>::Items([5, 6])
  match e {
    Vec2::Items(items) => { ret items[1] }
    Vec2::Empty => { ret 0 }
  }
}
