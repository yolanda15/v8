// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_MAGLEV_ARM64_MAGLEV_ASSEMBLER_ARM64_INL_H_
#define V8_MAGLEV_ARM64_MAGLEV_ASSEMBLER_ARM64_INL_H_

#include "src/codegen/interface-descriptors-inl.h"
#include "src/codegen/macro-assembler-inl.h"
#include "src/compiler/compilation-dependencies.h"
#include "src/maglev/maglev-assembler.h"
#include "src/maglev/maglev-basic-block.h"
#include "src/maglev/maglev-code-gen-state.h"

namespace v8 {
namespace internal {
namespace maglev {

constexpr Condition ConditionForFloat64(Operation operation) {
  return ConditionFor(operation);
}

constexpr Condition ConditionForNaN() { return vs; }

inline int ShiftFromScale(int n) {
  switch (n) {
    case 1:
      return 0;
    case 2:
      return 1;
    case 4:
      return 2;
    case 8:
      return 3;
    default:
      UNREACHABLE();
  }
}

class MaglevAssembler::ScratchRegisterScope {
 public:
  explicit ScratchRegisterScope(MaglevAssembler* masm)
      : wrapped_scope_(masm),
        masm_(masm),
        prev_scope_(masm->scratch_register_scope_) {
    masm_->scratch_register_scope_ = this;
  }

  ~ScratchRegisterScope() { masm_->scratch_register_scope_ = prev_scope_; }

  void ResetToDefault() {
    wrapped_scope_.SetAvailable(masm_->DefaultTmpList());
    wrapped_scope_.SetAvailableFP(masm_->DefaultFPTmpList());
  }

  Register GetDefaultScratchRegister() { return Acquire(); }
  DoubleRegister GetDefaultScratchDoubleRegister() { return AcquireDouble(); }

  Register Acquire() { return wrapped_scope_.AcquireX(); }
  void Include(Register reg) { wrapped_scope_.Include(reg); }
  void Include(const RegList list) {
    wrapped_scope_.Include(CPURegList(kXRegSizeInBits, list));
  }

  DoubleRegister AcquireDouble() { return wrapped_scope_.AcquireD(); }
  void IncludeDouble(const DoubleRegList list) {
    wrapped_scope_.IncludeFP(CPURegList(kDRegSizeInBits, list));
  }

  RegList Available() {
    return RegList::FromBits(wrapped_scope_.Available()->bits());
  }
  void SetAvailable(RegList list) {
    wrapped_scope_.SetAvailable(CPURegList(kXRegSizeInBits, list));
  }

  DoubleRegList AvailableDouble() {
    uint64_t bits = wrapped_scope_.AvailableFP()->bits();
    // AvailableFP fits in a 32 bits word.
    DCHECK_LE(bits, std::numeric_limits<uint32_t>::max());
    return DoubleRegList::FromBits(static_cast<uint32_t>(bits));
  }
  void SetAvailableDouble(DoubleRegList list) {
    wrapped_scope_.SetAvailableFP(CPURegList(kDRegSizeInBits, list));
  }

 private:
  UseScratchRegisterScope wrapped_scope_;
  MaglevAssembler* masm_;
  ScratchRegisterScope* prev_scope_;
};

inline MapCompare::MapCompare(MaglevAssembler* masm, Register object,
                              size_t map_count)
    : masm_(masm), object_(object), map_count_(map_count) {
  map_ = masm_->scratch_register_scope()->Acquire();
  masm_->LoadMap(map_, object_);
  USE(map_count_);
}

void MapCompare::Generate(Handle<Map> map) {
  MaglevAssembler::ScratchRegisterScope temps(masm_);
  Register temp = temps.Acquire();
  masm_->Move(temp, map);
  masm_->CmpTagged(map_, temp);
}

Register MapCompare::GetMap() { return map_; }

int MapCompare::TemporaryCount(size_t map_count) { return 1; }

namespace detail {

template <typename Arg>
inline Register ToRegister(MaglevAssembler* masm,
                           MaglevAssembler::ScratchRegisterScope* scratch,
                           Arg arg) {
  Register reg = scratch->Acquire();
  masm->Move(reg, arg);
  return reg;
}
inline Register ToRegister(MaglevAssembler* masm,
                           MaglevAssembler::ScratchRegisterScope* scratch,
                           Register reg) {
  return reg;
}
inline Register ToRegister(MaglevAssembler* masm,
                           MaglevAssembler::ScratchRegisterScope* scratch,
                           const Input& input) {
  if (input.operand().IsConstant()) {
    Register reg = scratch->Acquire();
    input.node()->LoadToRegister(masm, reg);
    return reg;
  }
  const compiler::AllocatedOperand& operand =
      compiler::AllocatedOperand::cast(input.operand());
  if (operand.IsRegister()) {
    return ToRegister(input);
  } else {
    DCHECK(operand.IsStackSlot());
    Register reg = scratch->Acquire();
    masm->Move(reg, masm->ToMemOperand(input));
    return reg;
  }
}

template <typename... Args>
struct CountPushHelper;

template <>
struct CountPushHelper<> {
  static int Count() { return 0; }
};

template <typename Arg, typename... Args>
struct CountPushHelper<Arg, Args...> {
  static int Count(Arg arg, Args... args) {
    int arg_count = 1;
    if constexpr (is_iterator_range<Arg>::value) {
      arg_count = static_cast<int>(std::distance(arg.begin(), arg.end()));
    }
    return arg_count + CountPushHelper<Args...>::Count(args...);
  }
};

template <typename... Args>
struct PushAllHelper;

template <typename... Args>
inline void PushAll(MaglevAssembler* masm, Args... args) {
  PushAllHelper<Args...>::Push(masm, args...);
}

template <typename... Args>
inline void PushAllReverse(MaglevAssembler* masm, Args... args) {
  PushAllHelper<Args...>::PushReverse(masm, args...);
}

template <>
struct PushAllHelper<> {
  static void Push(MaglevAssembler* masm) {}
  static void PushReverse(MaglevAssembler* masm) {}
};

template <typename T, typename... Args>
inline void PushIterator(MaglevAssembler* masm, base::iterator_range<T> range,
                         Args... args) {
  using value_type = typename base::iterator_range<T>::value_type;
  for (auto iter = range.begin(), end = range.end(); iter != end; ++iter) {
    value_type val1 = *iter;
    ++iter;
    if (iter == end) {
      PushAll(masm, val1, args...);
      return;
    }
    value_type val2 = *iter;
    masm->Push(val1, val2);
  }
  PushAll(masm, args...);
}

template <typename T, typename... Args>
inline void PushIteratorReverse(MaglevAssembler* masm,
                                base::iterator_range<T> range, Args... args) {
  using value_type = typename base::iterator_range<T>::value_type;
  using difference_type = typename base::iterator_range<T>::difference_type;
  difference_type count = std::distance(range.begin(), range.end());
  DCHECK_GE(count, 0);
  auto iter = range.rbegin();
  auto end = range.rend();
  if (count % 2 != 0) {
    PushAllReverse(masm, *iter, args...);
    ++iter;
  } else {
    PushAllReverse(masm, args...);
  }
  while (iter != end) {
    value_type val1 = *iter;
    ++iter;
    value_type val2 = *iter;
    ++iter;
    masm->Push(val1, val2);
  }
}

template <typename Arg1, typename Arg2>
inline void PushAligned(MaglevAssembler* masm, Arg1 arg1, Arg2 arg2) {
  {
    // Push the first argument together with padding to ensure alignment.
    // The second argument is not pushed together with the first so we can
    // re-use any scratch registers used to materialise the first argument for
    // the second one.
    MaglevAssembler::ScratchRegisterScope temps(masm);
    masm->MacroAssembler::Push(ToRegister(masm, &temps, arg1), padreg);
  }
  {
    MaglevAssembler::ScratchRegisterScope temps(masm);
    masm->MacroAssembler::str(ToRegister(masm, &temps, arg2), MemOperand(sp));
  }
}

template <typename Arg>
struct PushAllHelper<Arg> {
  static void Push(MaglevAssembler* masm, Arg arg) {
    if constexpr (is_iterator_range<Arg>::value) {
      PushIterator(masm, arg);
    } else {
      FATAL("Unaligned push");
    }
  }
  static void PushReverse(MaglevAssembler* masm, Arg arg) {
    if constexpr (is_iterator_range<Arg>::value) {
      PushIteratorReverse(masm, arg);
    } else {
      PushAllReverse(masm, arg, padreg);
    }
  }
};

template <typename Arg1, typename Arg2, typename... Args>
struct PushAllHelper<Arg1, Arg2, Args...> {
  static void Push(MaglevAssembler* masm, Arg1 arg1, Arg2 arg2, Args... args) {
    if constexpr (is_iterator_range<Arg1>::value) {
      PushIterator(masm, arg1, arg2, args...);
    } else if constexpr (is_iterator_range<Arg2>::value) {
      if (arg2.begin() != arg2.end()) {
        auto val = *arg2.begin();
        PushAligned(masm, arg1, val);
        PushAll(masm,
                base::make_iterator_range(std::next(arg2.begin()), arg2.end()),
                args...);
      } else {
        PushAll(masm, arg1, args...);
      }
    } else {
      PushAligned(masm, arg1, arg2);
      PushAll(masm, args...);
    }
  }
  static void PushReverse(MaglevAssembler* masm, Arg1 arg1, Arg2 arg2,
                          Args... args) {
    if constexpr (is_iterator_range<Arg1>::value) {
      PushIteratorReverse(masm, arg1, arg2, args...);
    } else if constexpr (is_iterator_range<Arg2>::value) {
      if (arg2.begin() != arg2.end()) {
        auto val = *arg2.begin();
        PushAllReverse(
            masm,
            base::make_iterator_range(std::next(arg2.begin()), arg2.end()),
            args...);
        PushAligned(masm, val, arg1);
      } else {
        PushAllReverse(masm, arg1, args...);
      }
    } else {
      PushAllReverse(masm, args...);
      PushAligned(masm, arg2, arg1);
    }
  }
};

}  // namespace detail

template <typename... T>
void MaglevAssembler::Push(T... vals) {
  const int push_count = detail::CountPushHelper<T...>::Count(vals...);
  if (push_count % 2 == 0) {
    detail::PushAll(this, vals...);
  } else {
    detail::PushAll(this, padreg, vals...);
  }
}

template <typename... T>
void MaglevAssembler::PushReverse(T... vals) {
  detail::PushAllReverse(this, vals...);
}

inline void MaglevAssembler::BindJumpTarget(Label* label) {
  MacroAssembler::BindJumpTarget(label);
}

inline void MaglevAssembler::BindBlock(BasicBlock* block) {
  if (block->is_start_block_of_switch_case()) {
    BindJumpTarget(block->label());
  } else {
    Bind(block->label());
  }
}

inline void MaglevAssembler::SmiTagInt32AndSetFlags(Register dst,
                                                    Register src) {
  Adds(dst.W(), src.W(), src.W());
}

inline void MaglevAssembler::CheckInt32IsSmi(Register obj, Label* fail,
                                             Register scratch) {
  Adds(wzr, obj.W(), obj.W());
  JumpIf(kOverflow, fail);
}

inline void MaglevAssembler::MoveHeapNumber(Register dst, double value) {
  Mov(dst, Operand::EmbeddedHeapNumber(value));
}

inline Condition MaglevAssembler::IsRootConstant(Input input,
                                                 RootIndex root_index) {
  if (input.operand().IsRegister()) {
    CompareRoot(ToRegister(input), root_index);
  } else {
    DCHECK(input.operand().IsStackSlot());
    ScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    Ldr(scratch, ToMemOperand(input));
    CompareRoot(scratch, root_index);
  }
  return eq;
}

inline MemOperand MaglevAssembler::StackSlotOperand(StackSlot slot) {
  return MemOperand(fp, slot.index);
}

// TODO(Victorgomes): Unify this to use StackSlot struct.
inline MemOperand MaglevAssembler::GetStackSlot(
    const compiler::AllocatedOperand& operand) {
  return MemOperand(fp, GetFramePointerOffsetForStackSlot(operand));
}

inline MemOperand MaglevAssembler::ToMemOperand(
    const compiler::InstructionOperand& operand) {
  return GetStackSlot(compiler::AllocatedOperand::cast(operand));
}

inline MemOperand MaglevAssembler::ToMemOperand(const ValueLocation& location) {
  return ToMemOperand(location.operand());
}

inline void MaglevAssembler::BuildTypedArrayDataPointer(Register data_pointer,
                                                        Register object) {
  DCHECK_NE(data_pointer, object);
  LoadExternalPointerField(
      data_pointer,
      FieldMemOperand(object, JSTypedArray::kExternalPointerOffset));
  if (JSTypedArray::kMaxSizeInHeap == 0) return;
  ScratchRegisterScope scope(this);
  Register base = scope.Acquire();
  Ldr(base.W(), FieldMemOperand(object, JSTypedArray::kBasePointerOffset));
  Add(data_pointer, data_pointer, base);
}

inline MemOperand MaglevAssembler::TypedArrayElementOperand(
    Register data_pointer, Register index, int element_size) {
  Add(data_pointer, data_pointer,
      Operand(index, LSL, ShiftFromScale(element_size)));
  return MemOperand(data_pointer);
}

inline void MaglevAssembler::LoadTaggedFieldByIndex(Register result,
                                                    Register object,
                                                    Register index, int scale,
                                                    int offset) {
  if (scale == 1) {
    Add(result, object, index);
  } else {
    Add(result, object, Operand(index, LSL, ShiftFromScale(scale / 2)));
  }
  MacroAssembler::LoadTaggedField(result, FieldMemOperand(result, offset));
}

inline void MaglevAssembler::LoadBoundedSizeFromObject(Register result,
                                                       Register object,
                                                       int offset) {
  Move(result, FieldMemOperand(object, offset));
#ifdef V8_ENABLE_SANDBOX
  Lsr(result, result, kBoundedSizeShift);
#endif  // V8_ENABLE_SANDBOX
}

inline void MaglevAssembler::LoadExternalPointerField(Register result,
                                                      MemOperand operand) {
#ifdef V8_ENABLE_SANDBOX
  LoadSandboxedPointerField(result, operand);
#else
  Move(result, operand);
#endif
}

void MaglevAssembler::LoadFixedArrayElement(Register result, Register array,
                                            Register index) {
  if (v8_flags.debug_code) {
    AssertNotSmi(array);
    IsObjectType(array, FIXED_ARRAY_TYPE);
    Assert(kEqual, AbortReason::kUnexpectedValue);
    CompareInt32(index, 0);
    Assert(kUnsignedGreaterThanEqual, AbortReason::kUnexpectedNegativeValue);
  }
  Add(result, array, Operand(index, LSL, kTaggedSizeLog2));
  DecompressTagged(result, FieldMemOperand(result, FixedArray::kHeaderSize));
}

void MaglevAssembler::LoadFixedArrayElementWithoutDecompressing(
    Register result, Register array, Register index) {
  if (v8_flags.debug_code) {
    AssertNotSmi(array);
    IsObjectType(array, FIXED_ARRAY_TYPE);
    Assert(kEqual, AbortReason::kUnexpectedValue);
    CompareInt32(index, 0);
    Assert(kUnsignedGreaterThanEqual, AbortReason::kUnexpectedNegativeValue);
  }
  Add(result, array, Operand(index, LSL, kTaggedSizeLog2));
  Ldr(result.W(), FieldMemOperand(result, FixedArray::kHeaderSize));
}

void MaglevAssembler::LoadFixedDoubleArrayElement(DoubleRegister result,
                                                  Register array,
                                                  Register index) {
  MaglevAssembler::ScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  if (v8_flags.debug_code) {
    AssertNotSmi(array);
    IsObjectType(array, FIXED_DOUBLE_ARRAY_TYPE);
    Assert(kEqual, AbortReason::kUnexpectedValue);
    CompareInt32(index, 0);
    Assert(kUnsignedGreaterThanEqual, AbortReason::kUnexpectedNegativeValue);
  }
  Add(scratch, array, Operand(index, LSL, kDoubleSizeLog2));
  Ldr(result, FieldMemOperand(scratch, FixedArray::kHeaderSize));
}

inline void MaglevAssembler::LoadSignedField(Register result,
                                             MemOperand operand, int size) {
  if (size == 1) {
    Ldrsb(result, operand);
  } else if (size == 2) {
    Ldrsh(result, operand);
  } else {
    DCHECK_EQ(size, 4);
    Ldr(result.W(), operand);
  }
}

inline void MaglevAssembler::LoadUnsignedField(Register result,
                                               MemOperand operand, int size) {
  if (size == 1) {
    Ldrb(result.W(), operand);
  } else if (size == 2) {
    Ldrh(result.W(), operand);
  } else {
    DCHECK_EQ(size, 4);
    Ldr(result.W(), operand);
  }
}

inline void MaglevAssembler::SetSlotAddressForTaggedField(Register slot_reg,
                                                          Register object,
                                                          int offset) {
  Add(slot_reg, object, offset - kHeapObjectTag);
}
inline void MaglevAssembler::SetSlotAddressForFixedArrayElement(
    Register slot_reg, Register object, Register index) {
  Add(slot_reg, object, FixedArray::kHeaderSize - kHeapObjectTag);
  Add(slot_reg, slot_reg, Operand(index, LSL, kTaggedSizeLog2));
}

inline void MaglevAssembler::StoreTaggedFieldNoWriteBarrier(Register object,
                                                            int offset,
                                                            Register value) {
  MacroAssembler::StoreTaggedField(value, FieldMemOperand(object, offset));
}

inline void MaglevAssembler::StoreFixedArrayElementNoWriteBarrier(
    Register array, Register index, Register value) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  Add(scratch, array, Operand(index, LSL, kTaggedSizeLog2));
  Str(value.W(), FieldMemOperand(scratch, FixedArray::kHeaderSize));
}

inline void MaglevAssembler::StoreTaggedSignedField(Register object, int offset,
                                                    Register value) {
  AssertSmi(value);
  MacroAssembler::StoreTaggedField(value, FieldMemOperand(object, offset));
}

inline void MaglevAssembler::StoreTaggedSignedField(Register object, int offset,
                                                    Smi value) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  Mov(scratch, value);
  MacroAssembler::StoreTaggedField(scratch, FieldMemOperand(object, offset));
}

inline void MaglevAssembler::StoreInt32Field(Register object, int offset,
                                             int32_t value) {
  if (value == 0) {
    Str(wzr, FieldMemOperand(object, offset));
    return;
  }
  ScratchRegisterScope scope(this);
  Register scratch = scope.Acquire().W();
  Move(scratch, value);
  Str(scratch, FieldMemOperand(object, offset));
}

inline void MaglevAssembler::StoreField(MemOperand operand, Register value,
                                        int size) {
  DCHECK(size == 1 || size == 2 || size == 4);
  if (size == 1) {
    Strb(value.W(), operand);
  } else if (size == 2) {
    Strh(value.W(), operand);
  } else {
    DCHECK_EQ(size, 4);
    Str(value.W(), operand);
  }
}

inline void MaglevAssembler::ReverseByteOrder(Register value, int size) {
  if (size == 2) {
    Rev16(value, value);
  } else if (size == 4) {
    Rev32(value, value);
  } else {
    DCHECK_EQ(size, 1);
  }
}

inline void MaglevAssembler::IncrementInt32(Register reg) {
  Add(reg.W(), reg.W(), Immediate(1));
}

inline void MaglevAssembler::Move(StackSlot dst, Register src) {
  Str(src, StackSlotOperand(dst));
}
inline void MaglevAssembler::Move(StackSlot dst, DoubleRegister src) {
  Str(src, StackSlotOperand(dst));
}
inline void MaglevAssembler::Move(Register dst, StackSlot src) {
  Ldr(dst, StackSlotOperand(src));
}
inline void MaglevAssembler::Move(DoubleRegister dst, StackSlot src) {
  Ldr(dst, StackSlotOperand(src));
}
inline void MaglevAssembler::Move(MemOperand dst, Register src) {
  Str(src, dst);
}
inline void MaglevAssembler::Move(Register dst, MemOperand src) {
  Ldr(dst, src);
}
inline void MaglevAssembler::Move(DoubleRegister dst, DoubleRegister src) {
  Fmov(dst, src);
}
inline void MaglevAssembler::Move(Register dst, Smi src) {
  MacroAssembler::Move(dst, src);
}
inline void MaglevAssembler::Move(Register dst, ExternalReference src) {
  Mov(dst, src);
}
inline void MaglevAssembler::Move(Register dst, Register src) {
  MacroAssembler::Move(dst, src);
}
inline void MaglevAssembler::Move(Register dst, TaggedIndex i) {
  Mov(dst, i.ptr());
}
inline void MaglevAssembler::Move(Register dst, int32_t i) {
  Mov(dst.W(), Immediate(i));
}
inline void MaglevAssembler::Move(DoubleRegister dst, double n) {
  Fmov(dst, n);
}
inline void MaglevAssembler::Move(DoubleRegister dst, Float64 n) {
  Fmov(dst, n.get_scalar());
}
inline void MaglevAssembler::Move(Register dst, Handle<HeapObject> obj) {
  Mov(dst, Operand(obj));
}

inline void MaglevAssembler::LoadFloat32(DoubleRegister dst, MemOperand src) {
  Ldr(dst.S(), src);
  Fcvt(dst, dst.S());
}
inline void MaglevAssembler::StoreFloat32(MemOperand dst, DoubleRegister src) {
  ScratchRegisterScope temps(this);
  DoubleRegister scratch = temps.AcquireDouble();
  Fcvt(scratch.S(), src);
  Str(scratch.S(), dst);
}
inline void MaglevAssembler::LoadFloat64(DoubleRegister dst, MemOperand src) {
  Ldr(dst, src);
}
inline void MaglevAssembler::StoreFloat64(MemOperand dst, DoubleRegister src) {
  Str(src, dst);
}

inline void MaglevAssembler::SignExtend32To64Bits(Register dst, Register src) {
  Mov(dst, Operand(src.W(), SXTW));
}
inline void MaglevAssembler::NegateInt32(Register val) {
  Neg(val.W(), val.W());
}

inline void MaglevAssembler::ToUint8Clamped(Register result,
                                            DoubleRegister value, Label* min,
                                            Label* max, Label* done) {
  ScratchRegisterScope temps(this);
  DoubleRegister scratch = temps.AcquireDouble();
  Move(scratch, 0.0);
  Fcmp(scratch, value);
  // Set to 0 if NaN.
  B(vs, min);
  B(ge, min);
  Move(scratch, 255.0);
  Fcmp(value, scratch);
  B(ge, max);
  // if value in [0, 255], then round up to the nearest.
  Frintn(scratch, value);
  TruncateDoubleToInt32(result, scratch);
  B(done);
}

template <typename NodeT>
inline void MaglevAssembler::DeoptIfBufferDetached(Register array,
                                                   Register scratch,
                                                   NodeT* node) {
  if (!code_gen_state()
           ->broker()
           ->dependencies()
           ->DependOnArrayBufferDetachingProtector()) {
    // A detached buffer leads to megamorphic feedback, so we won't have a deopt
    // loop if we deopt here.
    LoadTaggedField(scratch,
                    FieldMemOperand(array, JSArrayBufferView::kBufferOffset));
    LoadTaggedField(scratch,
                    FieldMemOperand(scratch, JSArrayBuffer::kBitFieldOffset));
    Tst(scratch.W(), Immediate(JSArrayBuffer::WasDetachedBit::kMask));
    EmitEagerDeoptIf(ne, DeoptimizeReason::kArrayBufferWasDetached, node);
  }
}

inline void MaglevAssembler::LoadByte(Register dst, MemOperand src) {
  Ldrb(dst, src);
}

inline Condition MaglevAssembler::IsCallableAndNotUndetectable(
    Register map, Register scratch) {
  Ldr(scratch.W(), FieldMemOperand(map, Map::kBitFieldOffset));
  And(scratch.W(), scratch.W(),
      Map::Bits1::IsUndetectableBit::kMask | Map::Bits1::IsCallableBit::kMask);
  Cmp(scratch.W(), Map::Bits1::IsCallableBit::kMask);
  return kEqual;
}

inline Condition MaglevAssembler::IsNotCallableNorUndetactable(
    Register map, Register scratch) {
  Ldr(scratch.W(), FieldMemOperand(map, Map::kBitFieldOffset));
  Tst(scratch.W(), Immediate(Map::Bits1::IsUndetectableBit::kMask |
                             Map::Bits1::IsCallableBit::kMask));
  return kEqual;
}

inline void MaglevAssembler::IsObjectType(Register heap_object,
                                          InstanceType type) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  MacroAssembler::IsObjectType(heap_object, scratch, scratch, type);
}

inline void MaglevAssembler::CompareObjectType(Register heap_object,
                                               InstanceType type) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  CompareObjectType(heap_object, type, scratch);
}

inline void MaglevAssembler::JumpIfJSAnyIsNotPrimitive(
    Register heap_object, Label* target, Label::Distance distance) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  MacroAssembler::JumpIfJSAnyIsNotPrimitive(heap_object, scratch, target,
                                            distance);
}

inline void MaglevAssembler::CompareObjectType(Register heap_object,
                                               InstanceType type,
                                               Register scratch) {
  LoadMap(scratch, heap_object);
  MacroAssembler::CompareInstanceType(scratch, scratch, type);
}

inline void MaglevAssembler::CompareObjectTypeRange(Register heap_object,
                                                    InstanceType lower_limit,
                                                    InstanceType higher_limit) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  CompareObjectTypeRange(heap_object, scratch, lower_limit, higher_limit);
}

inline void MaglevAssembler::CompareObjectTypeRange(Register heap_object,
                                                    Register scratch,
                                                    InstanceType lower_limit,
                                                    InstanceType higher_limit) {
  LoadMap(scratch, heap_object);
  CompareInstanceTypeRange(scratch, scratch, lower_limit, higher_limit);
}

inline void MaglevAssembler::CompareMapWithRoot(Register object,
                                                RootIndex index,
                                                Register scratch) {
  if (V8_STATIC_ROOTS_BOOL && RootsTable::IsReadOnly(index)) {
    Ldr(scratch.W(), FieldMemOperand(object, HeapObject::kMapOffset));
    CmpTagged(scratch, Immediate(ReadOnlyRootPtr(index)));
    return;
  }
  LoadMap(scratch, object);
  CompareRoot(scratch, index);
}

inline void MaglevAssembler::CompareInstanceType(Register map,
                                                 InstanceType instance_type) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  MacroAssembler::CompareInstanceType(map, scratch, instance_type);
}

inline void MaglevAssembler::CompareInstanceTypeRange(
    Register map, Register instance_type_out, InstanceType lower_limit,
    InstanceType higher_limit) {
  MacroAssembler::CompareInstanceTypeRange(map, instance_type_out, lower_limit,
                                           higher_limit);
}

inline void MaglevAssembler::CompareTagged(Register reg, Smi smi) {
  CmpTagged(reg, Immediate(smi));
}

inline void MaglevAssembler::CompareTagged(Register reg,
                                           Handle<HeapObject> obj) {
  CmpTagged(reg, Operand(obj, RelocInfo::COMPRESSED_EMBEDDED_OBJECT));
}

inline void MaglevAssembler::CompareTagged(Register src1, Register src2) {
  CmpTagged(src1, src2);
}

inline void MaglevAssembler::CompareInt32(Register reg, int32_t imm) {
  Cmp(reg.W(), Immediate(imm));
}

inline void MaglevAssembler::CompareInt32(Register src1, Register src2) {
  Cmp(src1.W(), src2.W());
}

inline void MaglevAssembler::CompareFloat64(DoubleRegister src1,
                                            DoubleRegister src2) {
  Fcmp(src1, src2);
}

inline void MaglevAssembler::PrepareCallCFunction(int num_reg_arguments,
                                                  int num_double_registers) {}

inline void MaglevAssembler::CallSelf() {
  DCHECK(allow_call());
  DCHECK(code_gen_state()->entry_label()->is_bound());
  Bl(code_gen_state()->entry_label());
}

inline void MaglevAssembler::Jump(Label* target, Label::Distance) { B(target); }

inline void MaglevAssembler::JumpIf(Condition cond, Label* target,
                                    Label::Distance) {
  B(target, cond);
}

inline void MaglevAssembler::JumpIfRoot(Register with, RootIndex index,
                                        Label* if_equal,
                                        Label::Distance distance) {
  MacroAssembler::JumpIfRoot(with, index, if_equal);
}

inline void MaglevAssembler::JumpIfNotRoot(Register with, RootIndex index,
                                           Label* if_not_equal,
                                           Label::Distance distance) {
  MacroAssembler::JumpIfNotRoot(with, index, if_not_equal);
}

inline void MaglevAssembler::JumpIfSmi(Register src, Label* on_smi,
                                       Label::Distance distance) {
  MacroAssembler::JumpIfSmi(src, on_smi);
}

inline void MaglevAssembler::JumpIfNotSmi(Register src, Label* on_smi,
                                          Label::Distance distance) {
  MacroAssembler::JumpIfNotSmi(src, on_smi);
}

void MaglevAssembler::JumpIfByte(Condition cc, Register value, int32_t byte,
                                 Label* target, Label::Distance) {
  CompareAndBranch(value, Immediate(byte), cc, target);
}

void MaglevAssembler::JumpIfHoleNan(DoubleRegister value, Register scratch,
                                    Label* target, Label::Distance distance) {
  MaglevAssembler::ScratchRegisterScope temps(this);
  Register repr = temps.Acquire();
  Mov(repr, value, 0);
  Mov(scratch, kHoleNanInt64);
  Cmp(repr, scratch);
  JumpIf(kEqual, target, distance);
}

void MaglevAssembler::JumpIfNotHoleNan(DoubleRegister value, Register scratch,
                                       Label* target,
                                       Label::Distance distance) {
  MaglevAssembler::ScratchRegisterScope temps(this);
  Register repr = temps.Acquire();
  Mov(repr, value, 0);
  Mov(scratch, kHoleNanInt64);
  Cmp(repr, scratch);
  JumpIf(kNotEqual, target, distance);
}

void MaglevAssembler::JumpIfNotHoleNan(MemOperand operand, Label* target,
                                       Label::Distance distance) {
  MaglevAssembler::ScratchRegisterScope temps(this);
  Register repr = temps.Acquire();
  Ldr(repr, operand);
  // Acquire {scratch} after Ldr, since this might need a scratch register.
  Register scratch = temps.Acquire();
  Mov(scratch, kHoleNanInt64);
  Cmp(repr, scratch);
  JumpIf(kNotEqual, target, distance);
}

inline void MaglevAssembler::CompareInt32AndJumpIf(Register r1, Register r2,
                                                   Condition cond,
                                                   Label* target,
                                                   Label::Distance distance) {
  CompareAndBranch(r1.W(), r2.W(), cond, target);
}

inline void MaglevAssembler::CompareInt32AndJumpIf(Register r1, int32_t value,
                                                   Condition cond,
                                                   Label* target,
                                                   Label::Distance distance) {
  CompareAndBranch(r1.W(), Immediate(value), cond, target);
}

inline void MaglevAssembler::CompareSmiAndJumpIf(Register r1, Smi value,
                                                 Condition cond, Label* target,
                                                 Label::Distance distance) {
  AssertSmi(r1);
  CompareTaggedAndBranch(r1, Immediate(value), cond, target);
}

inline void MaglevAssembler::CompareTaggedAndJumpIf(Register r1, Smi value,
                                                    Condition cond,
                                                    Label* target,
                                                    Label::Distance distance) {
  CompareTaggedAndBranch(r1, Immediate(value), cond, target);
}

inline void MaglevAssembler::CompareDoubleAndJumpIfZeroOrNaN(
    DoubleRegister reg, Label* target, Label::Distance distance) {
  Fcmp(reg, 0.0);
  JumpIf(eq, target);
  JumpIf(vs, target);  // NaN check
}

inline void MaglevAssembler::CompareDoubleAndJumpIfZeroOrNaN(
    MemOperand operand, Label* target, Label::Distance distance) {
  ScratchRegisterScope temps(this);
  DoubleRegister value_double = temps.AcquireDouble();
  Ldr(value_double, operand);
  CompareDoubleAndJumpIfZeroOrNaN(value_double, target, distance);
}

inline void MaglevAssembler::TestInt32AndJumpIfAnySet(
    Register r1, int32_t mask, Label* target, Label::Distance distance) {
  TestAndBranchIfAnySet(r1.W(), mask, target);
}

inline void MaglevAssembler::TestInt32AndJumpIfAnySet(
    MemOperand operand, int32_t mask, Label* target, Label::Distance distance) {
  ScratchRegisterScope temps(this);
  Register value = temps.Acquire().W();
  Ldr(value, operand);
  TestAndBranchIfAnySet(value, mask, target);
}

inline void MaglevAssembler::TestInt32AndJumpIfAllClear(
    Register r1, int32_t mask, Label* target, Label::Distance distance) {
  TestAndBranchIfAllClear(r1.W(), mask, target);
}

inline void MaglevAssembler::TestInt32AndJumpIfAllClear(
    MemOperand operand, int32_t mask, Label* target, Label::Distance distance) {
  ScratchRegisterScope temps(this);
  Register value = temps.Acquire().W();
  Ldr(value, operand);
  TestAndBranchIfAllClear(value, mask, target);
}

inline void MaglevAssembler::LoadHeapNumberValue(DoubleRegister result,
                                                 Register heap_number) {
  Ldr(result, FieldMemOperand(heap_number, HeapNumber::kValueOffset));
}

inline void MaglevAssembler::Int32ToDouble(DoubleRegister result,
                                           Register src) {
  Scvtf(result, src.W());
}

inline void MaglevAssembler::Uint32ToDouble(DoubleRegister result,
                                            Register src) {
  Ucvtf(result, src.W());
}

inline void MaglevAssembler::Pop(Register dst) { Pop(dst, padreg); }

inline void MaglevAssembler::AssertStackSizeCorrect() {
  if (v8_flags.debug_code) {
    ScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    Add(scratch, sp,
        RoundUp<2 * kSystemPointerSize>(
            code_gen_state()->stack_slots() * kSystemPointerSize +
            StandardFrameConstants::kFixedFrameSizeFromFp));
    Cmp(scratch, fp);
    Assert(eq, AbortReason::kStackAccessBelowStackPointer);
  }
}

inline void MaglevAssembler::FinishCode() {
  ForceConstantPoolEmissionWithoutJump();
}

template <typename NodeT>
inline void MaglevAssembler::EmitEagerDeoptIfNotEqual(DeoptimizeReason reason,
                                                      NodeT* node) {
  EmitEagerDeoptIf(ne, reason, node);
}

template <>
inline void MaglevAssembler::MoveRepr(MachineRepresentation repr, Register dst,
                                      Register src) {
  Mov(dst, src);
}
template <>
inline void MaglevAssembler::MoveRepr(MachineRepresentation repr, Register dst,
                                      MemOperand src) {
  switch (repr) {
    case MachineRepresentation::kWord32:
      return Ldr(dst.W(), src);
    case MachineRepresentation::kTagged:
    case MachineRepresentation::kTaggedPointer:
    case MachineRepresentation::kTaggedSigned:
      return Ldr(dst, src);
    default:
      UNREACHABLE();
  }
}
template <>
inline void MaglevAssembler::MoveRepr(MachineRepresentation repr,
                                      MemOperand dst, Register src) {
  switch (repr) {
    case MachineRepresentation::kWord32:
      return Str(src.W(), dst);
    case MachineRepresentation::kTagged:
    case MachineRepresentation::kTaggedPointer:
    case MachineRepresentation::kTaggedSigned:
      return Str(src, dst);
    default:
      UNREACHABLE();
  }
}
template <>
inline void MaglevAssembler::MoveRepr(MachineRepresentation repr,
                                      MemOperand dst, MemOperand src) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  MoveRepr(repr, scratch, src);
  MoveRepr(repr, dst, scratch);
}

}  // namespace maglev
}  // namespace internal
}  // namespace v8

#endif  // V8_MAGLEV_ARM64_MAGLEV_ASSEMBLER_ARM64_INL_H_
