// A deeply nested enum payload: the layout of the whole nest is
// computed once per level, not once per level per level.
fn main() -> i32 {
  x :=
    Option::Some(
      Option::Some(
        Option::Some(
          Option::Some(
            Option::Some(
              Option::Some(
                Option::Some(
                  Option::Some(
                    Option::Some(
                      Option::Some(
                        Option::Some(
                          Option::Some(
                            Option::Some(
                              Option::Some(
                                Option::Some(
                                  Option::Some(
                                    Option::Some(
                                      Option::Some(
                                        Option::Some(
                                          Option::Some(
                                            Option::Some(
                                              Option::Some(Option::Some(Option::Some(1)))
                                            )
                                          )
                                        )
                                      )
                                    )
                                  )
                                )
                              )
                            )
                          )
                        )
                      )
                    )
                  )
                )
              )
            )
          )
        )
      )
    )
  ret match x {
    Option::Some(_) => 0,
    Option::None => 1,
  }
}
