// Copyright 2011 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HANDLES_MAYBE_HANDLES_H_
#define V8_HANDLES_MAYBE_HANDLES_H_

#include <type_traits>

#include "src/handles/handles.h"

namespace v8 {
namespace internal {

struct NullMaybeHandleType {};

constexpr NullMaybeHandleType kNullMaybeHandle;

// ----------------------------------------------------------------------------
// A Handle can be converted into a MaybeHandle. Converting a MaybeHandle
// into a Handle requires checking that it does not point to nullptr.  This
// ensures nullptr checks before use.
//
// Also note that Handles do not provide default equality comparison or hashing
// operators on purpose. Such operators would be misleading, because intended
// semantics is ambiguous between Handle location and object identity.
template <typename T>
class MaybeHandle final {
 public:
  V8_INLINE MaybeHandle() = default;

  V8_INLINE MaybeHandle(NullMaybeHandleType) {}

  // Constructor for handling automatic up casting from Handle.
  // Ex. Handle<JSArray> can be passed when MaybeHandle<Object> is expected.
  template <typename S, typename = typename std::enable_if<
                            std::is_convertible<S*, T*>::value>::type>
  V8_INLINE MaybeHandle(Handle<S> handle) : location_(handle.location_) {}

  // Constructor for handling automatic up casting.
  // Ex. MaybeHandle<JSArray> can be passed when Handle<Object> is expected.
  template <typename S, typename = typename std::enable_if<
                            std::is_convertible<S*, T*>::value>::type>
  V8_INLINE MaybeHandle(MaybeHandle<S> maybe_handle)
      : location_(maybe_handle.location_) {}

  V8_INLINE MaybeHandle(T object, Isolate* isolate);
  V8_INLINE MaybeHandle(T object, LocalHeap* local_heap);

  V8_INLINE void Assert() const { DCHECK_NOT_NULL(location_); }
  V8_INLINE void Check() const { CHECK_NOT_NULL(location_); }

  V8_INLINE Handle<T> ToHandleChecked() const {
    Check();
    return Handle<T>(location_);
  }

  // Convert to a Handle with a type that can be upcasted to.
  template <typename S>
  V8_WARN_UNUSED_RESULT V8_INLINE bool ToHandle(Handle<S>* out) const {
    if (location_ == nullptr) {
      *out = Handle<T>::null();
      return false;
    } else {
      *out = Handle<T>(location_);
      return true;
    }
  }

  // Location equality.
  bool equals(MaybeHandle<T> other) const {
    return address() == other.address();
  }

  // Returns the raw address where this handle is stored. This should only be
  // used for hashing handles; do not ever try to dereference it.
  V8_INLINE Address address() const {
    return reinterpret_cast<Address>(location_);
  }

  bool is_null() const { return location_ == nullptr; }

 protected:
  Address* location_ = nullptr;

  // MaybeHandles of different classes are allowed to access each
  // other's location_.
  template <typename>
  friend class MaybeHandle;
};

template <typename T>
std::ostream& operator<<(std::ostream& os, MaybeHandle<T> handle);

// A handle which contains a potentially weak pointer. Keeps it alive (strongly)
// while the MaybeObjectHandle is alive.
class MaybeObjectHandle {
 public:
  inline MaybeObjectHandle()
      : reference_type_(HeapObjectReferenceType::STRONG) {}
  inline MaybeObjectHandle(MaybeObject object, Isolate* isolate);
  inline MaybeObjectHandle(Object object, Isolate* isolate);
  inline MaybeObjectHandle(MaybeObject object, LocalHeap* local_heap);
  inline MaybeObjectHandle(Object object, LocalHeap* local_heap);
  inline explicit MaybeObjectHandle(Handle<Object> object);

  static inline MaybeObjectHandle Weak(Object object, Isolate* isolate);
  static inline MaybeObjectHandle Weak(Handle<Object> object);

  inline MaybeObject operator*() const;
  inline MaybeObject operator->() const;
  inline Handle<Object> object() const;

  inline bool is_identical_to(const MaybeObjectHandle& other) const;
  bool is_null() const { return handle_.is_null(); }

 private:
  inline MaybeObjectHandle(Object object,
                           HeapObjectReferenceType reference_type,
                           Isolate* isolate);
  inline MaybeObjectHandle(Handle<Object> object,
                           HeapObjectReferenceType reference_type);

  HeapObjectReferenceType reference_type_;
  MaybeHandle<Object> handle_;
};

#ifdef V8_ENABLE_CONSERVATIVE_STACK_SCANNING

template <typename T>
class MaybeDirectHandle final {
 public:
  V8_INLINE MaybeDirectHandle() = default;

  V8_INLINE MaybeDirectHandle(NullMaybeHandleType) {}

  // Constructor for handling automatic up casting from DirectHandle.
  // Ex. DirectHandle<JSArray> can be passed when MaybeDirectHandle<Object> is
  // expected.
  template <typename S, typename = typename std::enable_if<
                            std::is_convertible<S*, T*>::value>::type>
  V8_INLINE MaybeDirectHandle(DirectHandle<S> handle)
      : location_(handle.location_) {}

  // Constructor for handling automatic up casting.
  // Ex. MaybeDirectHandle<JSArray> can be passed when DirectHandle<Object> is
  // expected.
  template <typename S, typename = typename std::enable_if<
                            std::is_convertible<S*, T*>::value>::type>
  V8_INLINE MaybeDirectHandle(MaybeDirectHandle<S> maybe_handle)
      : location_(maybe_handle.location_) {}

  V8_INLINE MaybeDirectHandle(T object, Isolate* isolate);
  V8_INLINE MaybeDirectHandle(T object, LocalHeap* local_heap);

  V8_INLINE void Assert() const { DCHECK_NE(location_, kTaggedNullAddress); }
  V8_INLINE void Check() const { CHECK_NE(location_, kTaggedNullAddress); }

  V8_INLINE DirectHandle<T> ToDirectHandleChecked() const {
    Check();
    return DirectHandle<T>(location_);
  }

  // Convert to a DirectHandle with a type that can be upcasted to.
  template <typename S>
  V8_WARN_UNUSED_RESULT V8_INLINE bool ToDirectHandle(
      DirectHandle<S>* out) const {
    if (location_ == kTaggedNullAddress) {
      *out = DirectHandle<T>::null();
      return false;
    } else {
      *out = DirectHandle<T>(location_);
      return true;
    }
  }

  // Returns the raw address where this direct handle is stored.
  V8_INLINE Address address() const { return location_; }

  bool is_null() const { return location_ == kTaggedNullAddress; }

 protected:
  Address location_ = kTaggedNullAddress;

  // MaybeDirectHandles of different classes are allowed to access each
  // other's location_.
  template <typename>
  friend class MaybeDirectHandle;
};

#endif  // V8_ENABLE_CONSERVATIVE_STACK_SCANNING

}  // namespace internal
}  // namespace v8

#endif  // V8_HANDLES_MAYBE_HANDLES_H_
