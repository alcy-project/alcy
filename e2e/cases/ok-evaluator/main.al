// A tiny evaluator: a value is a literal or a name, a name looks its
// number up in the environment, and a missing name is an error.
enum Value {
  Num(i64),
  Name(String),
}

fn eval(v: &Value, env: &Map<i64>) -> Result<i64, String> {
  match v {
    Value::Num(n) => {
      ret Result::Ok(*n)
    }
    Value::Name(name) => {
      match env.get(name.as_str()) {
        Option::Some(found) => {
          ret Result::Ok(*found)
        }
        Option::None => {
          ret Result::Err(String::from("unknown name"))
        }
      }
    }
  }
}

fn main() -> i32 {
  mut env := Map::<i64>::new()
  _ := env.insert("x", 10)
  a := Value::Name(String::from("x"))
  match eval(&a, &env) {
    Result::Ok(v) => {
      if v != 10 {
        ret 1
      }
    }
    Result::Err(_) => {
      ret 2
    }
  }
  b := Value::Num(5)
  match eval(&b, &env) {
    Result::Ok(v) => {
      if v != 5 {
        ret 3
      }
    }
    Result::Err(_) => {
      ret 4
    }
  }
  missing := Value::Name(String::from("y"))
  match eval(&missing, &env) {
    Result::Ok(_) => {
      ret 5
    }
    Result::Err(why) => {
      if why.as_str() != "unknown name" {
        ret 6
      }
    }
  }
  ret 0
}
