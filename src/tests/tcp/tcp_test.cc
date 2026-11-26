// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string_view>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "tcp/connection.h"
#include "tcp/endpoint.h"
#include "tcp/init.h"
#include "tcp/listener.h"

namespace tcp {

TEST_CASE("Only loopback hosts are accepted for an unauthenticated listener") {
  CHECK(is_loopback(""));
  CHECK(is_loopback("localhost"));
  CHECK(is_loopback("127.0.0.1"));
  CHECK(is_loopback("::1"));
  CHECK(!is_loopback("0.0.0.0"));
  CHECK(!is_loopback("example.com"));
}

TEST_CASE("A loopback connection carries bytes in both directions") {
  Init init;
  CHECK(init.ok());
  if (!init.ok()) {
    return;
  }

  u16 port = 0;
  auto listener = Listener::listen(Endpoint{"127.0.0.1", 0}, &port);
  if (!listener.is_ok() || port == 0) {
    return;
  }

  auto client = Connection::connect(Endpoint{"127.0.0.1", port});
  CHECK(client.is_ok());
  if (!client.is_ok()) {
    return;
  }
  auto accepted = listener.value().accept();
  CHECK(accepted.is_ok());
  if (!accepted.is_ok()) {
    return;
  }
  listener.value().close();

  CHECK(client.value().write("ping").is_ok());
  char received[4] = {};
  auto count = accepted.value().read(received);
  CHECK(count.is_ok());
  if (!count.is_ok()) {
    return;
  }
  CHECK(count.value() == 4);
  CHECK(std::string_view(received, count.value()) == "ping");

  CHECK(accepted.value().write("pong").is_ok());
  char answer[4] = {};
  count = client.value().read(answer);
  CHECK(count.is_ok());
  if (!count.is_ok()) {
    return;
  }
  CHECK(count.value() == 4);
  CHECK(std::string_view(answer, count.value()) == "pong");

  accepted.value().close();
  count = client.value().read(received);
  CHECK(count.is_ok());
  if (!count.is_ok()) {
    return;
  }
  CHECK(count.value() == 0);
}

}  // namespace tcp
