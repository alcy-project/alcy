// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tcp/tcp.h"

#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "config/build_config.h"

#if BUILD_FLAG(IS_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "tcp/endpoint.h"
#include "tcp/error.h"

namespace tcp {

namespace {

#if BUILD_FLAG(IS_OS_WIN)
using Native = SOCKET;
constexpr Native NATIVE_INVALID = INVALID_SOCKET;
#else
using Native = i32;
constexpr Native NATIVE_INVALID = -1;
#endif

struct Resolved {
  addrinfo* head = nullptr;

  explicit Resolved(addrinfo* value) : head(value) {}

  ~Resolved() {
    if (head != nullptr) {
      freeaddrinfo(head);
    }
  }
  Resolved(Resolved&& other) noexcept : head(other.head) {
    other.head = nullptr;
  }
  Resolved& operator=(Resolved&& other) noexcept {
    if (this != &other) {
      if (head != nullptr) {
        freeaddrinfo(head);
      }
      head = other.head;
      other.head = nullptr;
    }
    return *this;
  }
  Resolved(const Resolved&) = delete;
  Resolved& operator=(const Resolved&) = delete;
};

base::Result<Resolved, Error> resolve(const Endpoint& endpoint,
                                      bool listening) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (listening) {
    hints.ai_flags = AI_PASSIVE;
  }
  const std::string host =
      endpoint.host.empty() ? std::string("127.0.0.1") : endpoint.host;
  const std::string service = std::to_string(endpoint.port);
  addrinfo* result = nullptr;
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0) {
    return base::make_err(Error::ResolveFailed);
  }
  return base::make_ok(Resolved{result});
}

Handle opened(addrinfo& address) {
  const Native native = static_cast<Native>(
      ::socket(address.ai_family, address.ai_socktype, address.ai_protocol));
  if (native == NATIVE_INVALID) {
    return INVALID_HANDLE;
  }
  return static_cast<Handle>(native);
}

}  // namespace

Platform::Platform() {
#if BUILD_FLAG(IS_OS_WIN)
  WSADATA data{};
  ok_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
}

Platform::~Platform() {
#if BUILD_FLAG(IS_OS_WIN)
  if (ok_) {
    ::WSACleanup();
  }
#endif
}

base::Result<Handle, Error> open_listener(const Endpoint& endpoint,
                                          u16* bound_port) {
  base::Result<Resolved, Error> resolved = resolve(endpoint, true);
  if (resolved.is_err()) {
    return base::make_err(std::move(resolved).unwrap_err());
  }
  const Resolved& addresses = resolved.value();
  for (addrinfo* at = addresses.head; at != nullptr; at = at->ai_next) {
    const Handle handle = opened(*at);
    if (handle == INVALID_HANDLE) {
      continue;
    }
    constexpr i32 REUSE = 1;
#if BUILD_FLAG(IS_OS_WIN)
    const char* value = reinterpret_cast<const char*>(&REUSE);
#else
    const void* value = &REUSE;
#endif
    (void)::setsockopt(static_cast<Native>(handle), SOL_SOCKET, SO_REUSEADDR,
                       value, sizeof(REUSE));
    if (::bind(static_cast<Native>(handle), at->ai_addr,
               static_cast<i32>(at->ai_addrlen)) != 0) {
      close(handle);
      continue;
    }
    if (::listen(static_cast<Native>(handle), 1) != 0) {
      close(handle);
      return base::make_err(Error::ListenFailed);
    }
    if (bound_port != nullptr && endpoint.port == 0) {
      sockaddr_storage stored{};
      socklen_t length = sizeof(stored);
      if (::getsockname(static_cast<Native>(handle),
                        reinterpret_cast<sockaddr*>(&stored), &length) == 0) {
        if (stored.ss_family == AF_INET) {
          const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&stored);
          *bound_port = static_cast<u16>(ntohs(ipv4->sin_port));
        } else {
          const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&stored);
          *bound_port = static_cast<u16>(ntohs(ipv6->sin6_port));
        }
      }
    }
    return base::make_ok(handle);
  }
  return base::make_err(Error::BindFailed);
}

base::Result<Handle, Error> accept(Handle listener) {
  const Handle handle = static_cast<Handle>(
      ::accept(static_cast<Native>(listener), nullptr, nullptr));
  if (handle == INVALID_HANDLE) {
    return base::make_err(Error::AcceptFailed);
  }
  return base::make_ok(handle);
}

base::Result<Handle, Error> connect(const Endpoint& endpoint) {
  base::Result<Resolved, Error> resolved = resolve(endpoint, false);
  if (resolved.is_err()) {
    return base::make_err(std::move(resolved).unwrap_err());
  }
  const Resolved& addresses = resolved.value();
  for (addrinfo* at = addresses.head; at != nullptr; at = at->ai_next) {
    const Handle handle = opened(*at);
    if (handle == INVALID_HANDLE) {
      continue;
    }
    if (::connect(static_cast<Native>(handle), at->ai_addr,
                  static_cast<i32>(at->ai_addrlen)) == 0) {
      return base::make_ok(handle);
    }
    close(handle);
  }
  return base::make_err(Error::ConnectFailed);
}

base::Result<usize, Error> read(Handle handle, std::span<char> bytes) {
  if (bytes.empty()) {
    return base::make_ok(usize{0});
  }
#if BUILD_FLAG(IS_OS_WIN)
  const isize count = ::recv(static_cast<Native>(handle), bytes.data(),
                             static_cast<i32>(bytes.size()), 0);
#else
  const isize count =
      ::read(static_cast<Native>(handle), bytes.data(), bytes.size());
#endif
  if (count < 0) {
    return base::make_err(Error::ReadFailed);
  }
  return base::make_ok(static_cast<usize>(count));
}

base::Result<void, Error> write(Handle handle, std::string_view bytes) {
  usize written = 0;
  while (written < bytes.size()) {
#if BUILD_FLAG(IS_OS_WIN)
    const isize count =
        ::send(static_cast<Native>(handle), bytes.data() + written,
               static_cast<i32>(bytes.size() - written), 0);
#else
    const isize count = ::write(static_cast<Native>(handle),
                                bytes.data() + written, bytes.size() - written);
#endif
    if (count < 0) {
      return base::make_err(Error::WriteFailed);
    }
    if (count == 0) {
      return base::make_err(Error::Disconnected);
    }
    written += static_cast<usize>(count);
  }
  return base::make_ok();
}

void close(Handle handle) {
  if (handle == INVALID_HANDLE) {
    return;
  }
#if BUILD_FLAG(IS_OS_WIN)
  (void)::closesocket(static_cast<Native>(handle));
#else
  (void)::close(static_cast<Native>(handle));
#endif
}

}  // namespace tcp
