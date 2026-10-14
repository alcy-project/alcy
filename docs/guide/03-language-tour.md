# A tour of the language

This tour follows the pieces of alcy the compiler accepts today. Every
program here is complete, and every `alcy` block on this page was
compiled with the playground's compiler when the site was built, so each
one runs there unchanged.

## Values and variables

`:=` declares a name and infers its type from the value. A name declared
that way cannot be assigned again; `mut` declares one that can:

```alcy
fn main() {
  mut total := 0
  step := 2
  total = total + 40
  total = total + step
  println("total = {}", total)
}
```

The types in play here are `i32` for integers, `bool` for conditions,
and `str` for text. Arithmetic is explicit: `+` on two `i32` values is
`i32`, and there are no implicit conversions between types.

## Functions

`fn` declares a function. Parameters and the return type are written
out, and `ret` returns a value:

```alcy
fn add(a: i32, b: i32) -> i32 {
  ret a + b
}

fn main() {
  println("add(2, 3) = {}", add(2, 3))
}
```

A function that never returns a value says nothing after its parameter
list; its body needs no `ret`, and a bare `ret` leaves it early. The
`main` in the examples above is one of those.

## Control flow

`if` chooses between blocks, with `else if` and `else` chaining as
expected:

```alcy
fn sign(x: i32) -> i32 {
  if x < 0 {
    ret -1
  } else if x > 0 {
    ret 1
  }
  ret 0
}

fn main() {
  println("{} {} {}", sign(-7), sign(0), sign(9))
}
```

`while` repeats while its condition holds, and `loop` repeats until a
`break` leaves it; `continue` starts the next round. Both are
statements, not expressions:

```alcy
fn main() {
  mut n := 1
  mut sum := 0
  while n <= 5 {
    if n == 3 {
      n = n + 1
      continue
    }
    sum = sum + n
    n = n + 1
  }
  println("sum = {}", sum)
}
```

`for` walks a head item by item. Over an array, `.iter()` borrows the
elements, so each `x` below is a reference and `*x` reads the value it
points at:

```alcy
fn main() {
  a := [1i32, 2i32, 3i32]
  mut total := 0
  for x in a.iter() {
    total = total + *x
  }
  println("total = {}", total)
}
```

A range counts instead of walking a collection: `for n in 0..<3` yields
`0`, `1`, and `2`, and `for n in 0..=3` includes the end.

## Tuples and structs

A tuple groups values of different types; `.0`, `.1`, and so on read
its fields:

```alcy
fn divmod(a: i32, b: i32) -> (i32, i32) {
  ret (a / b, a % b)
}

fn main() {
  result := divmod(17, 5)
  println("17 / 5 = {} remainder {}", result.0, result.1)
}
```

`struct` names a product type, and a struct literal names every field:

```alcy
struct Point { x: i32, y: i32 }

fn manhattan(p: Point) -> i32 {
  ret p.x + p.y
}

fn main() {
  p := Point { x: 3, y: 4 }
  println("manhattan = {}", manhattan(p))
}
```

## Enums and match

An `enum` is a sum of variants, and a variant may carry values. `match`
must cover every variant, and `|` groups patterns that share a body:

```alcy
enum Shape { Circle(i32), Square(i32), Rect(i32, i32) }

fn area(s: Shape) -> i32 {
  r := match s {
    Shape::Circle(radius) | Shape::Square(radius) => radius * radius * 3,
    Shape::Rect(w, h) => w * h,
  }
  ret r
}

fn main() {
  a := area(Shape::Circle(2))
  b := area(Shape::Rect(3, 4))
  println("circle = {}, rect = {}", a, b)
}
```

Enums may take type parameters, and the compiler understands `Option`
and `Result` well enough to destructure them: a condition binds a
pattern with `if pattern := expr`, and `?` propagates a failure:

```alcy
fn half(n: i32) -> Result<i32, str> {
  if n % 2 != 0 {
    ret Result::Err("odd")
  }
  ret Result::Ok(n / 2)
}

fn sum_of_halves(a: i32, b: i32) -> Result<i32, str> {
  x := half(a)?
  y := half(b)?
  ret Result::Ok(x + y)
}

fn main() {
  if Result::Ok(v) := sum_of_halves(8, 6) {
    println("8/2 + 6/2 = {}", v)
  }
}
```

## Closures

A closure is a function value written where it is used; a `[...]` list
in front of the parameters names what it takes from its surroundings. A
bare name captures by value, and for a `Copy` type like `i32` that
capture is a copy: `add` below keeps the `base` it saw when it was
made, so assigning 100 to `base` afterwards does not change it:

```alcy
fn main() {
  mut base := 10
  add := [base] (x: i32) -> x + base
  base = 100
  println("add(1) = {}, base = {}", add(1), base)
}
```

`&name` captures a shared borrow and `&mut name` an exclusive one, so
the closure reads or writes the live value instead of a copy.

A capture whose type owns memory moves in instead: the value keeps what
it took and ends it when the value ends, so a closure over a `String`
is a value like any other. A body may read what it captured and borrow
it, but not consume it, because the closure can run again.

## Printing

`print` and `println` take a compile-time format string and one
argument per `{}`, the same call shape as `format`, which builds the
expansion as an owned `String` instead of writing it. `print` writes
the expansion as it is; `println` writes it and then a newline:

```alcy
fn main() {
  print("print leaves the line open, ")
  println("println closes it")
  println("the answer is {}", 40 + 2)
}
```

A float placeholder can fix its fractional digits: `{:.2f}` keeps two
of them.

```alcy
fn main() {
  println("{:.2f}", 2.0 / 3.0)
  println("{}", 1.5)
}
```

Write the conversion out, because the shorthand `{:.2}` is the
same piece and the analyzer warns (WC045): the `f` says what the
placeholder formats without reading the argument beside it.

## Packages

A package describes itself in an `alcy.toml`, and its `[dependencies]`
table names what it needs with the version range that dependency may
drift within. Fetching them is still to come, but the manifest checks
what it reads: a pre-1.0 range warns (WH007), because a minor upgrade
may break the dependency, while an exact pin cannot drift and stays
silent:

```toml
[dependencies]
"hash/one" = { version = "0.5.x" }
```

Write `=0.5.0` in place of `0.5.x` to pin the version exactly.

## Where to go next

- [The principles](../principles.md) describe where the language is
  going and what decides design questions.
- [The specification](../spec/overview.md) is the normative description.
- [The architecture](../../compiler/docs/architecture.md) describes the
  compiler's design.

The language is far from finished: generic functions take no bounds
yet, a closure that captures borrows its frame, and several
standard-library packages are declared but empty. The
[backlog](../backlog.md) tracks what is being worked on.
