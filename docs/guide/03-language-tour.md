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
  t := (total,)
  print(format("total = {}\n", t).as_str())
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
  t := (add(2, 3),)
  print(format("add(2, 3) = {}\n", t).as_str())
}
```

A function that never returns a value says nothing after its parameter
list, and its body still returns with a bare `ret`. The `main` in the
examples above is one of those.

## Control flow

`if` chooses between blocks, with `else if` and `else` chaining as
expected:

```alcy
fn sign(x: i32) -> i32 {
  if x < 0 {
    ret 0 - 1
  } else if x > 0 {
    ret 1
  }
  ret 0
}

fn main() {
  t := (sign(-7), sign(0), sign(9))
  print(format("{} {} {}\n", t).as_str())
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
  t := (sum,)
  print(format("sum = {}\n", t).as_str())
}
```

## Tuples and structs

A tuple groups values of different types; `.0`, `.1`, and so on read
its fields:

```alcy
fn divmod(a: i32, b: i32) -> (i32, i32) {
  ret (a / b, a % b)
}

fn main() {
  result := divmod(17, 5)
  t := (result.0, result.1)
  print(format("17 / 5 = {} remainder {}\n", t).as_str())
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
  t := (manhattan(p),)
  print(format("manhattan = {}\n", t).as_str())
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
  t := (a, b)
  print(format("circle = {}, rect = {}\n", t).as_str())
}
```

Enums may take type parameters, and the compiler understands `Option`
and `Result` well enough to destructure them: `if let` tests one variant
and binds its payload, and `?` propagates the other one.

## Printing

`print` and `println` write a `str`. `format` builds one from a
compile-time format string and a tuple with one element per `{}`:

```alcy
fn main() {
  print("print leaves the line open, ")
  println("println closes it")
  line := format("{}\n", (40 + 2,))
  print(line.as_str())
}
```

`format` returns an owned `String`; `.as_str()` lends it wherever a
`str` is wanted. The tuple in `(40 + 2,)` needs its trailing comma to be
a one-element tuple rather than a parenthesized expression.

## Where to go next

- [The principles](../principles.md) describe where the language is
  going and what decides design questions.
- [The specification](../spec/overview.md) is the normative description.
- [The architecture](../../compiler/docs/architecture.md) describes the
  compiler's design.

The language is far from finished: generics are limited, closures and
modules exist only in part, and the standard library is mostly declared
rather than implemented. The [backlog](../backlog.md) tracks what is
being worked on.
