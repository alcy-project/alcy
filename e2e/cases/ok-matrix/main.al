// A row-major matrix over one flat vector, indexed by the spec.
struct Matrix {
  rows: usize,
  cols: usize,
  data: Vec<i64>,
}

impl Matrix {
  fn new(rows: usize, cols: usize) -> Matrix {
    mut data := Vec::<i64>::new()
    mut i := 0 as usize
    while i < rows * cols {
      data.push(0)
      i = i + 1
    }
    ret Matrix { rows: rows, cols: cols, data: data }
  }

  fn at(self: &Self, r: usize, c: usize) -> i64 {
    ret self.data[r * self.cols + c]
  }

  fn set(mut self: &mut Self, r: usize, c: usize, v: i64) {
    self.data[r * self.cols + c] = v
  }
}

fn main() -> i32 {
  mut m := Matrix::new(2, 3)
  m.set(1, 2, 7)
  if m.at(1, 2) != 7 {
    ret 1
  }
  if m.at(0, 0) != 0 {
    ret 2
  }
  ret 0
}
