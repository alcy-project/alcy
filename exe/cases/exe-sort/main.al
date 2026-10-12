// `Vec::sort` rides `Ord`, and `sort_by` takes the order as a
// predicate. The sort is a stable insertion sort: equal keys keep
// their relative order, and elements move by swapping through the
// buffer, so a move-only `T` never copies (its destructor runs once).

struct Entry {
  key: i32,
  tag: i32,
}

impl PartialEq for Entry {
  fn eq(self: &Self, other: &Self) -> bool {
    ret self.key == other.key
  }
}

impl Eq for Entry {
}

impl PartialOrd for Entry {
  fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering> {
    ret Option::Some(self.key.cmp(&other.key))
  }
}

impl Ord for Entry {
  fn cmp(self: &Self, other: &Self) -> Ordering {
    ret self.key.cmp(&other.key)
  }
}

// A move-only element: its destructor prints, so a double drop would
// show up as a repeated line.
struct Noisy {
  n: i32,
}

impl Noisy {
  fn drop(self: Noisy) {
    println("drop {}", self.n)
  }
}

fn main() -> i32 {
  mut v: Vec<Entry> := Vec::<Entry>::new()
  v.push(Entry { key: 2, tag: 0 })
  v.push(Entry { key: 1, tag: 1 })
  v.push(Entry { key: 2, tag: 2 })
  v.push(Entry { key: 1, tag: 3 })
  v.sort()
  println("{} {} {} {}", v[0].tag, v[1].tag, v[2].tag, v[3].tag)

  mut u: Vec<Entry> := Vec::<Entry>::new()
  u.push(Entry { key: 2, tag: 0 })
  u.push(Entry { key: 1, tag: 1 })
  u.push(Entry { key: 2, tag: 2 })
  u.sort_by((a, b) -> a.key < b.key)
  println("{} {} {}", u[0].tag, u[1].tag, u[2].tag)

  mut w: Vec<i32> := Vec::<i32>::new()
  w.push(5)
  w.push(2)
  w.push(9)
  w.sort_by((a, b) -> *a > *b)
  println("{} {} {}", w[0], w[1], w[2])

  mut n: Vec<Noisy> := Vec::<Noisy>::new()
  n.push(Noisy { n: 3 })
  n.push(Noisy { n: 1 })
  n.push(Noisy { n: 2 })
  n.sort_by((a, b) -> a.n < b.n)
  println("sorted")
  ret 0
}
