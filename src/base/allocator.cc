// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)

namespace {

// A block carries a sixteen byte header, so a payload handed back can be
// matched to the size class whose free list it belongs on, and the alignment
// arithmetic stays a single subtraction. Blocks above the largest class come
// straight from the system allocator.
constexpr std::size_t kAlign = 16;
constexpr std::size_t kSmallLimit = 256;
constexpr std::size_t kSmallStep = 16;
constexpr std::size_t kLargeLimit = 4096;
constexpr std::size_t kLargeStep = 64;
constexpr std::size_t kSmallClasses = kSmallLimit / kSmallStep;
constexpr std::size_t kClasses =
    kSmallClasses + (kLargeLimit - kSmallLimit) / kLargeStep;
constexpr std::size_t kChunk = std::size_t{64} << 10;
constexpr std::uint32_t kSystem = 0xFFFFFFFFu;

struct alignas(kAlign) Header {
  std::uint32_t class_index;
};

static_assert(sizeof(Header) == kAlign);

// Free lists and the bump region are per thread, and a chunk is never returned
// to the system, so a block freed on another thread is recycled here without
// touching the memory of the thread that allocated it.
thread_local void* free_lists[kClasses];
thread_local char* bump = nullptr;
thread_local char* bump_end = nullptr;

std::uint32_t class_of(std::size_t size) {
  if (size <= kSmallLimit) {
    return static_cast<std::uint32_t>((size - 1) / kSmallStep);
  }
  return static_cast<std::uint32_t>(kSmallClasses +
                                    (size - kSmallLimit - 1) / kLargeStep);
}

std::size_t block_size(std::uint32_t class_index) {
  if (class_index < kSmallClasses) {
    return (std::size_t{class_index} + 1) * kSmallStep;
  }
  return kSmallLimit +
         (std::size_t{class_index} - kSmallClasses + 1) * kLargeStep;
}

// A free list answers most requests, so the two paths behind it - a fresh
// block from the bump region and a block the system owns - stay out of the
// allocating function: no register it saves outlives the fast path, and the
// region refill it rarely takes is not competing for the cache lines it reads.
[[gnu::noinline, gnu::cold]] Header* carve(std::uint32_t class_index,
                                           std::size_t size) {
  if (class_index == kSystem) {
    // A block above the largest class is owned by the system allocator and
    // handed back by the matching free below.
    // ast-grep-ignore: no-c-heap
    auto* header = static_cast<Header*>(std::malloc(size + kAlign));
    if (header == nullptr) {
      std::abort();
    }
    header->class_index = kSystem;
    return header;
  }
  const std::size_t span = block_size(class_index) + kAlign;
  if (bump == nullptr || static_cast<std::size_t>(bump_end - bump) < span) {
    // ast-grep-ignore: no-c-heap
    bump = static_cast<char*>(std::malloc(kChunk));
    if (bump == nullptr) {
      std::abort();
    }
    bump_end = bump + kChunk;
  }
  auto* header = reinterpret_cast<Header*>(bump);
  bump += span;
  header->class_index = class_index;
  return header;
}

void* allocate(std::size_t size) {
  if (size == 0) {
    size = 1;
  }
  if (size > kLargeLimit) {
    return carve(kSystem, size) + 1;
  }
  const std::uint32_t class_index = class_of(size);
  if (void* block = free_lists[class_index]; block != nullptr) {
    free_lists[class_index] = *static_cast<void**>(block);
    return block;
  }
  return carve(class_index, size) + 1;
}

void deallocate(void* payload) noexcept {
  if (payload == nullptr) {
    return;
  }
  auto* header = static_cast<Header*>(payload) - 1;
  const std::uint32_t class_index = header->class_index;
  if (class_index == kSystem) {
    // ast-grep-ignore: no-c-heap
    std::free(header);
    return;
  }
  *static_cast<void**>(payload) = free_lists[class_index];
  free_lists[class_index] = payload;
}

}  // namespace

void* operator new(std::size_t size) {
  return allocate(size);
}

void* operator new[](std::size_t size) {
  return allocate(size);
}

void operator delete(void* payload) noexcept {
  deallocate(payload);
}

void operator delete[](void* payload) noexcept {
  deallocate(payload);
}

void operator delete(void* payload, std::size_t size) noexcept {
  static_cast<void>(size);
  deallocate(payload);
}

void operator delete[](void* payload, std::size_t size) noexcept {
  static_cast<void>(size);
  deallocate(payload);
}

#endif
