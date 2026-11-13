// A sequence ends the elements it drops from first to last (ADR-0086).

struct D {
  id: i32,
}

impl D {
  fn drop(self: D) {
    print("d{}\n", self.id)
  }
}

fn main() -> i32 {
  mut v: Vec<D> := Vec::<D>::new()
  v.push(D { id: 0 })
  v.push(D { id: 1 })
  v.push(D { id: 2 })
  v.push(D { id: 3 })
  print("push\n")
  v.truncate(2)
  print("truncate\n")
  mut u: Vec<D> := Vec::<D>::new()
  u.push(D { id: 4 })
  u.push(D { id: 5 })
  u.push(D { id: 6 })
  u.push(D { id: 7 })
  u.retain((d: &D) -> d.id % 2 == 0)
  print("retain\n")
  mut s: Vec<D> := Vec::<D>::new()
  s.push(D { id: 8 })
  s.push(D { id: 9 })
  s.push(D { id: 10 })
  s.push(D { id: 11 })
  s.push(D { id: 12 })
  s.dedup_by((a: &D, b: &D) -> a.id / 2 == b.id / 2)
  print("dedup\n")
  ret 0
}
