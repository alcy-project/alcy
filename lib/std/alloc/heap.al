// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/alloc`: the typed heap primitives.

// Heap allocation. `alloc<T>(count)` reserves room for `count`
// elements of `T`, using `T`'s own size and alignment. The returned
// reference uniquely owns the block and must be released with
// `dealloc<T>(ptr, count)`. `T` is fixed by the call's turbofish, so a
// call admits one instantiation.
pub unsafe intrinsic fn alloc<T>(count: usize) -> &mut MaybeUninit<T>;

pub unsafe intrinsic fn dealloc<T>(ptr: &mut MaybeUninit<T>, count: usize);

// Typed element offset. The result points at element `index` of the
// buffer `ptr` addresses, and is in bounds exactly when the caller keeps
// `index` within the buffer's length. Element type `T` is fixed by the
// pointee of `ptr`, so the call admits one instantiation.
pub unsafe intrinsic fn elem_ptr<T>(ptr: &mut MaybeUninit<T>, index: usize) -> &mut MaybeUninit<T>;

// Shared counterpart of `elem_ptr`, so a buffer is readable through a
// shared borrow of the value that owns it. `&mut T` coerces to `&T`, so
// the argument may name the buffer either way.
pub unsafe intrinsic fn elem_ref<T>(ptr: &MaybeUninit<T>, index: usize) -> &MaybeUninit<T>;

// Writes `value` into an uninitialized slot. Moving the value in leaves
// the slot initialized, so a later `uninit_assume` on it is sound.
pub intrinsic fn uninit_write<T>(slot: &mut MaybeUninit<T>, value: T);

// Releases a slot as a mutable reference to its value. Reading through
// the result before anything was written yields whatever the allocator
// returned.
pub unsafe intrinsic fn uninit_assume<T>(slot: &mut MaybeUninit<T>) -> &mut T;

// Shared counterpart of `uninit_assume`, with the same caveat: reading
// before anything was written yields whatever was there.
pub intrinsic fn uninit_ref<T>(slot: &MaybeUninit<T>) -> &T;
