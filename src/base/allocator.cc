// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdio>
#include <cstdlib>
#include <new>

#include "fpag/base/numeric.h"

// A sanitizer build keeps the system allocator: each sanitizer tracks the size
// and the lifetime of the block it handed out, and the recycled blocks a size
// class holds would hide that.
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && \
    !defined(__SANITIZE_MEMORY__)

namespace {

// A block carries a sixteen byte header, so a payload handed back can be
// matched to the size class whose free list it belongs on, and the alignment
// arithmetic stays a single subtraction. Blocks above the largest class come
// straight from the system allocator.
constexpr usize kAlign = 16;
constexpr usize kSmallLimit = 256;
constexpr usize kSmallStep = 16;
constexpr usize kLargeLimit = 4096;
constexpr usize kLargeStep = 64;
constexpr usize kSmallClasses = kSmallLimit / kSmallStep;
constexpr usize kClasses =
    kSmallClasses + (kLargeLimit - kSmallLimit) / kLargeStep;
constexpr usize kChunk = usize{64} << 10;
constexpr u32 kSystem = 0xFFFFFFFFu;

struct alignas(kAlign) Header {
  u32 class_index;
};

static_assert(sizeof(Header) == kAlign);

// Free lists and the bump region are per thread, and a chunk is never returned
// to the system, so a block freed on another thread is recycled here without
// touching the memory of the thread that allocated it.
thread_local void* free_lists[kClasses];
thread_local char* bump = nullptr;
thread_local char* bump_end = nullptr;

u32 class_of(usize size) {
  if (size <= kSmallLimit) {
    return static_cast<u32>((size - 1) / kSmallStep);
  }
  return static_cast<u32>(kSmallClasses +
                          (size - kSmallLimit - 1) / kLargeStep);
}

usize block_size(u32 class_index) {
  if (class_index < kSmallClasses) {
    return (usize{class_index} + 1) * kSmallStep;
  }
  return kSmallLimit + (usize{class_index} - kSmallClasses + 1) * kLargeStep;
}

// A free list answers most requests, so the two paths behind it - a fresh
// block from the bump region and a block the system owns - stay out of the
// allocating function: no register it saves outlives the fast path, and the
// region refill it rarely takes is not competing for the cache lines it reads.
[[gnu::noinline, gnu::cold]] Header* carve(u32 class_index, usize size) {
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
  const usize span = block_size(class_index) + kAlign;
  if (bump == nullptr || static_cast<usize>(bump_end - bump) < span) {
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

void* allocate(usize size) {
  if (size == 0) {
    size = 1;
  }
  if (size > kLargeLimit) {
    return carve(kSystem, size) + 1;
  }
  const u32 class_index = class_of(size);
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
  const u32 class_index = header->class_index;
  if (class_index != kSystem && class_index >= kClasses) {
    std::fprintf(stderr, "delete of a block this allocator never carved\n");
    std::abort();
  }
  if (class_index == kSystem) {
    // ast-grep-ignore: no-c-heap
    std::free(header);
    return;
  }
  *static_cast<void**>(payload) = free_lists[class_index];
  free_lists[class_index] = payload;
}

}  // namespace

void* operator new(usize size) {
  return allocate(size);
}

void* operator new[](usize size) {
  return allocate(size);
}

// With exceptions off, libc++'s nothrow forms call std::malloc directly
// instead of the throwing forms, so leaving them to the library pairs a
// malloc'd block with this allocator's delete. Every form this binary can
// pair must route through the same allocate/deallocate.
void* operator new(usize size, const std::nothrow_t&) noexcept {
  return allocate(size);
}

void* operator new[](usize size, const std::nothrow_t&) noexcept {
  return allocate(size);
}

void operator delete(void* payload) noexcept {
  deallocate(payload);
}

void operator delete[](void* payload) noexcept {
  deallocate(payload);
}

void operator delete(void* payload, usize size) noexcept {
  static_cast<void>(size);
  deallocate(payload);
}

void operator delete[](void* payload, usize size) noexcept {
  static_cast<void>(size);
  deallocate(payload);
}

void operator delete(void* payload, const std::nothrow_t&) noexcept {
  deallocate(payload);
}

void operator delete[](void* payload, const std::nothrow_t&) noexcept {
  deallocate(payload);
}

#endif
