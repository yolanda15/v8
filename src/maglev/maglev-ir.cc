// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/maglev/maglev-ir.h"

#include <limits>

#include "src/base/bounds.h"
#include "src/builtins/builtins-constructor.h"
#include "src/codegen/interface-descriptors-inl.h"
#include "src/codegen/interface-descriptors.h"
#include "src/compiler/heap-refs.h"
#include "src/deoptimizer/deoptimize-reason.h"
#include "src/execution/isolate-inl.h"
#include "src/heap/local-heap.h"
#include "src/heap/parked-scope.h"
#include "src/interpreter/bytecode-flags.h"
#include "src/maglev/maglev-assembler-inl.h"
#include "src/maglev/maglev-assembler.h"
#include "src/maglev/maglev-code-gen-state.h"
#include "src/maglev/maglev-compilation-unit.h"
#include "src/maglev/maglev-graph-labeller.h"
#include "src/maglev/maglev-graph-processor.h"
#include "src/maglev/maglev-ir-inl.h"
#include "src/roots/roots.h"

namespace v8 {
namespace internal {
namespace maglev {

#define __ masm->

const char* OpcodeToString(Opcode opcode) {
#define DEF_NAME(Name) #Name,
  static constexpr const char* const names[] = {NODE_BASE_LIST(DEF_NAME)};
#undef DEF_NAME
  return names[static_cast<int>(opcode)];
}

BasicBlock* Phi::predecessor_at(int i) {
  return merge_state_->predecessor_at(i);
}

namespace {

// Prevent people from accidentally using kScratchRegister here and having their
// code break in arm64.
struct Do_not_use_kScratchRegister_in_arch_independent_code {
} kScratchRegister;
struct Do_not_use_kScratchDoubleRegister_in_arch_independent_code {
} kScratchDoubleRegister;
static_assert(!std::is_same_v<decltype(kScratchRegister), Register>);
static_assert(
    !std::is_same_v<decltype(kScratchDoubleRegister), DoubleRegister>);

}  // namespace

#ifdef DEBUG
namespace {

template <size_t InputCount, typename Base, typename Derived>
int StaticInputCount(FixedInputNodeTMixin<InputCount, Base, Derived>*) {
  return InputCount;
}

int StaticInputCount(NodeBase*) { UNREACHABLE(); }

}  // namespace

void NodeBase::CheckCanOverwriteWith(Opcode new_opcode,
                                     OpProperties new_properties) {
  DCHECK_IMPLIES(new_properties.can_eager_deopt(),
                 properties().can_eager_deopt());
  DCHECK_IMPLIES(new_properties.can_lazy_deopt(),
                 properties().can_lazy_deopt());
  DCHECK_IMPLIES(new_properties.needs_register_snapshot(),
                 properties().needs_register_snapshot());

  int old_input_count = input_count();
  size_t old_sizeof = -1;
  switch (opcode()) {
#define CASE(op)             \
  case Opcode::k##op:        \
    old_sizeof = sizeof(op); \
    break;
    NODE_BASE_LIST(CASE);
#undef CASE
  }

  switch (new_opcode) {
#define CASE(op)                                                          \
  case Opcode::k##op: {                                                   \
    DCHECK_EQ(old_input_count, StaticInputCount(static_cast<op*>(this))); \
    DCHECK_EQ(sizeof(op), old_sizeof);                                    \
    break;                                                                \
  }
    NODE_BASE_LIST(CASE)
#undef CASE
  }
}

#endif  // DEBUG

bool Phi::is_loop_phi() const { return merge_state()->is_loop(); }

void Phi::RecordUseReprHint(UseRepresentationSet repr_mask,
                            int current_offset) {
  if (is_loop_phi() && merge_state()->loop_info()->Contains(current_offset)) {
    same_loop_uses_repr_hint_.Add(repr_mask);
  }

  if (!repr_mask.is_subset_of(uses_repr_hint_)) {
    uses_repr_hint_.Add(repr_mask);

    // Propagate in inputs, ignoring unbounded loop backedges.
    int bound_inputs = input_count();
    if (merge_state()->is_unmerged_loop()) --bound_inputs;

    for (int i = 0; i < bound_inputs; i++) {
      if (Phi* phi_input = input(i).node()->TryCast<Phi>()) {
        phi_input->RecordUseReprHint(repr_mask, current_offset);
      }
    }
  }
}

namespace {

// ---
// Print
// ---

void PrintInputs(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                 const NodeBase* node) {
  if (!node->has_inputs()) return;

  os << " [";
  for (int i = 0; i < node->input_count(); i++) {
    if (i != 0) os << ", ";
    graph_labeller->PrintInput(os, node->input(i));
  }
  os << "]";
}

void PrintResult(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                 const NodeBase* node) {}

void PrintResult(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                 const ValueNode* node) {
  os << " → " << node->result().operand();
  if (node->result().operand().IsAllocated() && node->is_spilled() &&
      node->spill_slot() != node->result().operand()) {
    os << " (spilled: " << node->spill_slot() << ")";
  }
  if (node->has_valid_live_range()) {
    os << ", live range: [" << node->live_range().start << "-"
       << node->live_range().end << "]";
  }
}

void PrintTargets(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                  const NodeBase* node) {}

void PrintTargets(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                  const UnconditionalControlNode* node) {
  os << " b" << graph_labeller->BlockId(node->target());
}

void PrintTargets(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                  const BranchControlNode* node) {
  os << " b" << graph_labeller->BlockId(node->if_true()) << " b"
     << graph_labeller->BlockId(node->if_false());
}

void PrintTargets(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                  const Switch* node) {
  for (int i = 0; i < node->size(); i++) {
    const BasicBlockRef& target = node->Cast<Switch>()->targets()[i];
    os << " b" << graph_labeller->BlockId(target.block_ptr());
  }
  if (node->Cast<Switch>()->has_fallthrough()) {
    BasicBlock* fallthrough_target = node->Cast<Switch>()->fallthrough();
    os << " b" << graph_labeller->BlockId(fallthrough_target);
  }
}

class MaybeUnparkForPrint {
 public:
  MaybeUnparkForPrint() {
    LocalHeap* local_heap = LocalHeap::Current();
    if (!local_heap) {
      local_heap = Isolate::Current()->main_thread_local_heap();
    }
    DCHECK_NOT_NULL(local_heap);
    if (local_heap->IsParked()) {
      scope_.emplace(local_heap);
    }
  }

 private:
  base::Optional<UnparkedScope> scope_;
};

template <typename NodeT>
void PrintImpl(std::ostream& os, MaglevGraphLabeller* graph_labeller,
               const NodeT* node, bool skip_targets) {
  MaybeUnparkForPrint unpark;
  os << node->opcode();
  node->PrintParams(os, graph_labeller);
  PrintInputs(os, graph_labeller, node);
  PrintResult(os, graph_labeller, node);
  if (!skip_targets) {
    PrintTargets(os, graph_labeller, node);
  }
}

size_t GetInputLocationsArraySize(const DeoptFrame& top_frame) {
  static constexpr int kClosureSize = 1;
  static constexpr int kReceiverSize = 1;
  static constexpr int kContextSize = 1;
  size_t size = 0;
  const DeoptFrame* frame = &top_frame;
  do {
    switch (frame->type()) {
      case DeoptFrame::FrameType::kInterpretedFrame:
        size += kClosureSize + frame->as_interpreted().frame_state()->size(
                                   frame->as_interpreted().unit());
        break;
      case DeoptFrame::FrameType::kInlinedArgumentsFrame:
        size += kClosureSize + frame->as_inlined_arguments().arguments().size();
        break;
      case DeoptFrame::FrameType::kConstructStubFrame:
        size += kClosureSize + kReceiverSize +
                frame->as_construct_stub().arguments_without_receiver().size() +
                kContextSize;
        break;
      case DeoptFrame::FrameType::kBuiltinContinuationFrame:
        size +=
            frame->as_builtin_continuation().parameters().size() + kContextSize;
        break;
    }
    frame = frame->parent();
  } while (frame != nullptr);
  return size;
}

bool RootToBoolean(RootIndex index) {
  switch (index) {
    case RootIndex::kFalseValue:
    case RootIndex::kNullValue:
    case RootIndex::kUndefinedValue:
    case RootIndex::kNanValue:
    case RootIndex::kHoleNanValue:
    case RootIndex::kMinusZeroValue:
    case RootIndex::kempty_string:
#ifdef V8_ENABLE_WEBASSEMBLY
    case RootIndex::kWasmNull:
#endif
      return false;
    default:
      return true;
  }
}

#ifdef DEBUG
// For all RO roots, check that RootToBoolean returns the same value as
// BooleanValue on that root.
bool CheckToBooleanOnAllRoots(LocalIsolate* local_isolate) {
  ReadOnlyRoots roots(local_isolate);
  // Use the READ_ONLY_ROOT_LIST macro list rather than a for loop to get nicer
  // error messages if there is a failure.
#define DO_CHECK(type, name, CamelName)                                   \
  /* Ignore 'undefined' roots that are not the undefined value itself. */ \
  if (roots.name() != roots.undefined_value() ||                          \
      RootIndex::k##CamelName == RootIndex::kUndefinedValue) {            \
    DCHECK_EQ(roots.name()->BooleanValue(local_isolate),                  \
              RootToBoolean(RootIndex::k##CamelName));                    \
  }
  READ_ONLY_ROOT_LIST(DO_CHECK)
#undef DO_CHECK
  return true;
}
#endif

}  // namespace

bool RootConstant::ToBoolean(LocalIsolate* local_isolate) const {
#ifdef DEBUG
  // (Ab)use static locals to call CheckToBooleanOnAllRoots once, on first
  // call to this function.
  static bool check_once = CheckToBooleanOnAllRoots(local_isolate);
  DCHECK(check_once);
#endif
  // ToBoolean is only supported for RO roots.
  DCHECK(RootsTable::IsReadOnly(index_));
  return RootToBoolean(index_);
}

bool FromConstantToBool(LocalIsolate* local_isolate, ValueNode* node) {
  DCHECK(IsConstantNode(node->opcode()));
  switch (node->opcode()) {
#define CASE(Name)                                       \
  case Opcode::k##Name: {                                \
    return node->Cast<Name>()->ToBoolean(local_isolate); \
  }
    CONSTANT_VALUE_NODE_LIST(CASE)
#undef CASE
    default:
      UNREACHABLE();
  }
}

bool FromConstantToBool(MaglevAssembler* masm, ValueNode* node) {
  // TODO(leszeks): Getting the main thread local isolate is not what we
  // actually want here, but it's all we have, and it happens to work because
  // really all we're using it for is ReadOnlyRoots. We should change ToBoolean
  // to be able to pass ReadOnlyRoots in directly.
  return FromConstantToBool(masm->isolate()->AsLocalIsolate(), node);
}

DeoptInfo::DeoptInfo(Zone* zone, const DeoptFrame top_frame,
                     compiler::FeedbackSource feedback_to_update)
    : top_frame_(top_frame),
      feedback_to_update_(feedback_to_update),
      input_locations_(zone->NewArray<InputLocation>(
          GetInputLocationsArraySize(top_frame))) {
  // Initialise InputLocations so that they correctly don't have a next use id.
  for (size_t i = 0; i < GetInputLocationsArraySize(top_frame); ++i) {
    new (&input_locations_[i]) InputLocation();
  }
}

bool LazyDeoptInfo::IsResultRegister(interpreter::Register reg) const {
  if (top_frame().type() == DeoptFrame::FrameType::kConstructStubFrame) {
    return reg == interpreter::Register::virtual_accumulator();
  }
  if (V8_LIKELY(result_size() == 1)) {
    return reg == result_location_;
  }
  if (result_size() == 0) {
    return false;
  }
  DCHECK_EQ(result_size(), 2);
  return reg == result_location_ ||
         reg == interpreter::Register(result_location_.index() + 1);
}

void NodeBase::Print(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                     bool skip_targets) const {
  switch (opcode()) {
#define V(Name)         \
  case Opcode::k##Name: \
    return PrintImpl(os, graph_labeller, this->Cast<Name>(), skip_targets);
    NODE_BASE_LIST(V)
#undef V
  }
  UNREACHABLE();
}

void NodeBase::Print() const {
  MaglevGraphLabeller labeller;
  Print(std::cout, &labeller);
  std::cout << std::endl;
}

void ValueNode::SetHint(compiler::InstructionOperand hint) {
  if (!hint_.IsInvalid()) return;
  hint_ = hint;
  if (result_.operand().IsUnallocated()) {
    auto operand = compiler::UnallocatedOperand::cast(result_.operand());
    if (operand.HasSameAsInputPolicy()) {
      input(operand.input_index()).node()->SetHint(hint);
    }
  }
  if (this->Is<Phi>()) {
    for (Input& input : *this) {
      if (input.node()->has_id() && input.node()->id() < this->id()) {
        input.node()->SetHint(hint);
      }
    }
  }
}

void ValueNode::SetNoSpill() {
  DCHECK(!IsConstantNode(opcode()));
#ifdef DEBUG
  state_ = kSpill;
#endif  // DEBUG
  spill_ = compiler::InstructionOperand();
}

void ValueNode::SetConstantLocation() {
  DCHECK(IsConstantNode(opcode()));
#ifdef DEBUG
  state_ = kSpill;
#endif  // DEBUG
  spill_ = compiler::ConstantOperand(
      compiler::UnallocatedOperand::cast(result().operand())
          .virtual_register());
}

// ---
// Check input value representation
// ---

ValueRepresentation ToValueRepresentation(MachineType type) {
  switch (type.representation()) {
    case MachineRepresentation::kTagged:
    case MachineRepresentation::kTaggedSigned:
    case MachineRepresentation::kTaggedPointer:
      return ValueRepresentation::kTagged;
    case MachineRepresentation::kFloat64:
      return ValueRepresentation::kFloat64;
    case MachineRepresentation::kWord64:
      return ValueRepresentation::kWord64;
    default:
      return ValueRepresentation::kInt32;
  }
}

void CheckValueInputIs(const NodeBase* node, int i,
                       ValueRepresentation expected,
                       MaglevGraphLabeller* graph_labeller) {
  ValueNode* input = node->input(i).node();
  DCHECK(!input->Is<Identity>());
  ValueRepresentation got = input->properties().value_representation();
  // Allow Float64 values to be inputs when HoleyFloat64 is expected.
  bool valid =
      (got == expected) || (got == ValueRepresentation::kFloat64 &&
                            expected == ValueRepresentation::kHoleyFloat64);
  if (!valid) {
    std::ostringstream str;
    str << "Type representation error: node ";
    if (graph_labeller) {
      str << "#" << graph_labeller->NodeId(node) << " : ";
    }
    str << node->opcode() << " (input @" << i << " = " << input->opcode()
        << ") type " << got << " is not " << expected;
    FATAL("%s", str.str().c_str());
  }
}

void CheckValueInputIs(const NodeBase* node, int i, Opcode expected,
                       MaglevGraphLabeller* graph_labeller) {
  ValueNode* input = node->input(i).node();
  Opcode got = input->opcode();
  if (got != expected) {
    std::ostringstream str;
    str << "Opcode error: node ";
    if (graph_labeller) {
      str << "#" << graph_labeller->NodeId(node) << " : ";
    }
    str << node->opcode() << " (input @" << i << " = " << input->opcode()
        << ") opcode " << got << " is not " << expected;
    FATAL("%s", str.str().c_str());
  }
}

void CheckValueInputIsWord32(const NodeBase* node, int i,
                             MaglevGraphLabeller* graph_labeller) {
  ValueNode* input = node->input(i).node();
  DCHECK(!input->Is<Identity>());
  ValueRepresentation got = input->properties().value_representation();
  if (got != ValueRepresentation::kInt32 &&
      got != ValueRepresentation::kUint32) {
    std::ostringstream str;
    str << "Type representation error: node ";
    if (graph_labeller) {
      str << "#" << graph_labeller->NodeId(node) << " : ";
    }
    str << node->opcode() << " (input @" << i << " = " << input->opcode()
        << ") type " << got << " is not Word32 (Int32 or Uint32)";
    FATAL("%s", str.str().c_str());
  }
}

void GeneratorStore::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void UnsafeSmiTag::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  DCHECK_EQ(input_count(), 1);
  CheckValueInputIsWord32(this, 0, graph_labeller);
}

void Phi::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  switch (value_representation()) {
#define CASE_REPR(repr)                                        \
  case ValueRepresentation::k##repr:                           \
    for (int i = 0; i < input_count(); i++) {                  \
      CheckValueInputIs(this, i, ValueRepresentation::k##repr, \
                        graph_labeller);                       \
    }                                                          \
    break;

    CASE_REPR(Tagged)
    CASE_REPR(Int32)
    CASE_REPR(Uint32)
    CASE_REPR(Float64)
    CASE_REPR(HoleyFloat64)
#undef CASE_REPR
    case ValueRepresentation::kWord64:
      UNREACHABLE();
  }
}

void Call::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void Call::MarkTaggedInputsAsDecompressing() {
  for (int i = 0; i < input_count(); i++) {
    input(i).node()->SetTaggedResultNeedsDecompress();
  }
}

void CallWithArrayLike::VerifyInputs(
    MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void CallWithArrayLike::MarkTaggedInputsAsDecompressing() {
  for (int i = 0; i < input_count(); i++) {
    input(i).node()->SetTaggedResultNeedsDecompress();
  }
}

void CallWithSpread::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void CallWithSpread::MarkTaggedInputsAsDecompressing() {
  for (int i = 0; i < input_count(); i++) {
    input(i).node()->SetTaggedResultNeedsDecompress();
  }
}

void CallSelf::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void CallSelf::MarkTaggedInputsAsDecompressing() {
  for (int i = 0; i < input_count(); i++) {
    input(i).node()->SetTaggedResultNeedsDecompress();
  }
}

void CallKnownJSFunction::VerifyInputs(
    MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void CallKnownJSFunction::MarkTaggedInputsAsDecompressing() {
  for (int i = 0; i < input_count(); i++) {
    input(i).node()->SetTaggedResultNeedsDecompress();
  }
}

void Construct::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void Construct::MarkTaggedInputsAsDecompressing() {
  for (int i = 0; i < input_count(); i++) {
    input(i).node()->SetTaggedResultNeedsDecompress();
  }
}

void ConstructWithSpread::VerifyInputs(
    MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void ConstructWithSpread::MarkTaggedInputsAsDecompressing() {
  for (int i = 0; i < input_count(); i++) {
    input(i).node()->SetTaggedResultNeedsDecompress();
  }
}

void CallBuiltin::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  auto descriptor = Builtins::CallInterfaceDescriptorFor(builtin());
  int count = input_count();
  // Verify context.
  if (descriptor.HasContextParameter()) {
    CheckValueInputIs(this, count - 1, ValueRepresentation::kTagged,
                      graph_labeller);
    count--;
  }

// {all_input_count} includes the feedback slot and vector.
#ifdef DEBUG
  int all_input_count = count + (has_feedback() ? 2 : 0);
  if (descriptor.AllowVarArgs()) {
    DCHECK_GE(all_input_count, descriptor.GetParameterCount());
  } else {
    DCHECK_EQ(all_input_count, descriptor.GetParameterCount());
  }
#endif
  int i = 0;
  // Check the rest of inputs.
  for (; i < count; ++i) {
    MachineType type = i < descriptor.GetParameterCount()
                           ? descriptor.GetParameterType(i)
                           : MachineType::AnyTagged();
    CheckValueInputIs(this, i, ToValueRepresentation(type), graph_labeller);
  }
}

void CallBuiltin::MarkTaggedInputsAsDecompressing() {
  auto descriptor = Builtins::CallInterfaceDescriptorFor(builtin());
  int count = input_count();
  // Set context.
  if (descriptor.HasContextParameter()) {
    input(count - 1).node()->SetTaggedResultNeedsDecompress();
    count--;
  }
  int i = 0;
  // Set the rest of the tagged inputs.
  for (; i < count; ++i) {
    MachineType type = i < descriptor.GetParameterCount()
                           ? descriptor.GetParameterType(i)
                           : MachineType::AnyTagged();
    if (type.IsTagged() && !type.IsTaggedSigned()) {
      input(i).node()->SetTaggedResultNeedsDecompress();
    }
  }
}

void CallRuntime::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void CallRuntime::MarkTaggedInputsAsDecompressing() {
  for (int i = 0; i < input_count(); i++) {
    input(i).node()->SetTaggedResultNeedsDecompress();
  }
}

void FoldedAllocation::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  Base::VerifyInputs(graph_labeller);
  CheckValueInputIs(this, 0, Opcode::kAllocateRaw, graph_labeller);
}

// ---
// Reify constants
// ---

Handle<Object> ValueNode::Reify(LocalIsolate* isolate) const {
  switch (opcode()) {
#define V(Name)         \
  case Opcode::k##Name: \
    return this->Cast<Name>()->DoReify(isolate);
    CONSTANT_VALUE_NODE_LIST(V)
#undef V
    default:
      UNREACHABLE();
  }
}

Handle<Object> ExternalConstant::DoReify(LocalIsolate* isolate) const {
  UNREACHABLE();
}

Handle<Object> SmiConstant::DoReify(LocalIsolate* isolate) const {
  return handle(value_, isolate);
}

Handle<Object> Int32Constant::DoReify(LocalIsolate* isolate) const {
  return isolate->factory()->NewNumber<AllocationType::kOld>(value());
}

Handle<Object> Float64Constant::DoReify(LocalIsolate* isolate) const {
  return isolate->factory()->NewNumber<AllocationType::kOld>(
      value_.get_scalar());
}

Handle<Object> Constant::DoReify(LocalIsolate* isolate) const {
  return object_.object();
}

Handle<Object> RootConstant::DoReify(LocalIsolate* isolate) const {
  return isolate->root_handle(index());
}

// ---
// Load node to registers
// ---

namespace {
template <typename NodeT>
void LoadToRegisterHelper(NodeT* node, MaglevAssembler* masm, Register reg) {
  if constexpr (!IsDoubleRepresentation(
                    NodeT::kProperties.value_representation())) {
    return node->DoLoadToRegister(masm, reg);
  } else {
    UNREACHABLE();
  }
}
template <typename NodeT>
void LoadToRegisterHelper(NodeT* node, MaglevAssembler* masm,
                          DoubleRegister reg) {
  if constexpr (IsDoubleRepresentation(
                    NodeT::kProperties.value_representation())) {
    return node->DoLoadToRegister(masm, reg);
  } else {
    UNREACHABLE();
  }
}
}  // namespace

void ValueNode::LoadToRegister(MaglevAssembler* masm, Register reg) {
  switch (opcode()) {
#define V(Name)         \
  case Opcode::k##Name: \
    return LoadToRegisterHelper(this->Cast<Name>(), masm, reg);
    VALUE_NODE_LIST(V)
#undef V
    default:
      UNREACHABLE();
  }
}
void ValueNode::LoadToRegister(MaglevAssembler* masm, DoubleRegister reg) {
  switch (opcode()) {
#define V(Name)         \
  case Opcode::k##Name: \
    return LoadToRegisterHelper(this->Cast<Name>(), masm, reg);
    VALUE_NODE_LIST(V)
#undef V
    default:
      UNREACHABLE();
  }
}

void ValueNode::DoLoadToRegister(MaglevAssembler* masm, Register reg) {
  DCHECK(is_spilled());
  DCHECK(!use_double_register());
  __ Move(reg,
          masm->GetStackSlot(compiler::AllocatedOperand::cast(spill_slot())));
}

void ValueNode::DoLoadToRegister(MaglevAssembler* masm, DoubleRegister reg) {
  DCHECK(is_spilled());
  DCHECK(use_double_register());
  __ LoadFloat64(
      reg, masm->GetStackSlot(compiler::AllocatedOperand::cast(spill_slot())));
}

void ExternalConstant::DoLoadToRegister(MaglevAssembler* masm, Register reg) {
  __ Move(reg, reference());
}

void SmiConstant::DoLoadToRegister(MaglevAssembler* masm, Register reg) {
  __ Move(reg, value());
}

void Int32Constant::DoLoadToRegister(MaglevAssembler* masm, Register reg) {
  __ Move(reg, value());
}

void Float64Constant::DoLoadToRegister(MaglevAssembler* masm,
                                       DoubleRegister reg) {
  __ Move(reg, value());
}

void Constant::DoLoadToRegister(MaglevAssembler* masm, Register reg) {
  __ Move(reg, object_.object());
}

void RootConstant::DoLoadToRegister(MaglevAssembler* masm, Register reg) {
  __ LoadRoot(reg, index());
}

// ---
// Arch agnostic nodes
// ---

void ExternalConstant::SetValueLocationConstraints() { DefineAsConstant(this); }
void ExternalConstant::GenerateCode(MaglevAssembler* masm,
                                    const ProcessingState& state) {}

void SmiConstant::SetValueLocationConstraints() { DefineAsConstant(this); }
void SmiConstant::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {}

void Int32Constant::SetValueLocationConstraints() { DefineAsConstant(this); }
void Int32Constant::GenerateCode(MaglevAssembler* masm,
                                 const ProcessingState& state) {}

void Float64Constant::SetValueLocationConstraints() { DefineAsConstant(this); }
void Float64Constant::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {}

void Constant::SetValueLocationConstraints() { DefineAsConstant(this); }
void Constant::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {}

void RootConstant::SetValueLocationConstraints() { DefineAsConstant(this); }
void RootConstant::GenerateCode(MaglevAssembler* masm,
                                const ProcessingState& state) {}

InitialValue::InitialValue(uint64_t bitfield, interpreter::Register source)
    : Base(bitfield), source_(source) {}

void InitialValue::SetValueLocationConstraints() {
  result().SetUnallocated(compiler::UnallocatedOperand::FIXED_SLOT,
                          stack_slot(), kNoVreg);
}
void InitialValue::GenerateCode(MaglevAssembler* masm,
                                const ProcessingState& state) {
  // No-op, the value is already in the appropriate slot.
}

// static
uint32_t InitialValue::stack_slot(uint32_t register_index) {
  // TODO(leszeks): Make this nicer.
  return (StandardFrameConstants::kExpressionsOffset -
          UnoptimizedFrameConstants::kRegisterFileFromFp) /
             kSystemPointerSize +
         register_index;
}

uint32_t InitialValue::stack_slot() const {
  return stack_slot(source_.index());
}

void RegisterInput::SetValueLocationConstraints() {
  DefineAsFixed(this, input());
}
void RegisterInput::GenerateCode(MaglevAssembler* masm,
                                 const ProcessingState& state) {
  // Nothing to be done, the value is already in the register.
}

void GetSecondReturnedValue::SetValueLocationConstraints() {
  DefineAsFixed(this, kReturnRegister1);
}
void GetSecondReturnedValue::GenerateCode(MaglevAssembler* masm,
                                          const ProcessingState& state) {
  // No-op. This is just a hack that binds kReturnRegister1 to a value node.
  // kReturnRegister1 is guaranteed to be free in the register allocator, since
  // previous node in the basic block is a call.
#ifdef DEBUG
  // Check if the previous node is call.
  Node* previous = nullptr;
  for (Node* node : state.block()->nodes()) {
    if (node == this) {
      break;
    }
    previous = node;
  }
  DCHECK_NE(previous, nullptr);
  DCHECK(previous->properties().is_call());
#endif  // DEBUG
}

void Deopt::SetValueLocationConstraints() {}
void Deopt::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {
  __ EmitEagerDeopt(this, reason());
}

void Phi::SetValueLocationConstraints() {
  for (Input& input : *this) {
    UseAny(input);
  }

  // We have to pass a policy for the result, but it is ignored during register
  // allocation. See StraightForwardRegisterAllocator::AllocateRegisters which
  // has special handling for Phis.
  static const compiler::UnallocatedOperand::ExtendedPolicy kIgnoredPolicy =
      compiler::UnallocatedOperand::REGISTER_OR_SLOT_OR_CONSTANT;

  result().SetUnallocated(kIgnoredPolicy, kNoVreg);
}
void Phi::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {}

namespace {

constexpr Builtin BuiltinFor(Operation operation) {
  switch (operation) {
#define CASE(name)         \
  case Operation::k##name: \
    return Builtin::k##name##_WithFeedback;
    OPERATION_LIST(CASE)
#undef CASE
  }
}

}  // namespace

template <class Derived, Operation kOperation>
void UnaryWithFeedbackNode<Derived, kOperation>::SetValueLocationConstraints() {
  using D = UnaryOp_WithFeedbackDescriptor;
  UseFixed(operand_input(), D::GetRegisterParameter(D::kValue));
  DefineAsFixed(this, kReturnRegister0);
}

template <class Derived, Operation kOperation>
void UnaryWithFeedbackNode<Derived, kOperation>::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  __ CallBuiltin<BuiltinFor(kOperation)>(
      masm->native_context().object(),  // context
      operand_input(),                  // value
      feedback().index(),               // feedback slot
      feedback().vector                 // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

template <class Derived, Operation kOperation>
void BinaryWithFeedbackNode<Derived,
                            kOperation>::SetValueLocationConstraints() {
  using D = BinaryOp_WithFeedbackDescriptor;
  UseFixed(left_input(), D::GetRegisterParameter(D::kLeft));
  UseFixed(right_input(), D::GetRegisterParameter(D::kRight));
  DefineAsFixed(this, kReturnRegister0);
}

template <class Derived, Operation kOperation>
void BinaryWithFeedbackNode<Derived, kOperation>::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  __ CallBuiltin<BuiltinFor(kOperation)>(
      masm->native_context().object(),  // context
      left_input(),                     // left
      right_input(),                    // right
      feedback().index(),               // feedback slot
      feedback().vector                 // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

#define DEF_OPERATION(Name)                               \
  void Name::SetValueLocationConstraints() {              \
    Base::SetValueLocationConstraints();                  \
  }                                                       \
  void Name::GenerateCode(MaglevAssembler* masm,          \
                          const ProcessingState& state) { \
    Base::GenerateCode(masm, state);                      \
  }
GENERIC_OPERATIONS_NODE_LIST(DEF_OPERATION)
#undef DEF_OPERATION

void ConstantGapMove::SetValueLocationConstraints() { UNREACHABLE(); }

namespace {
template <typename T>
struct GetRegister;
template <>
struct GetRegister<Register> {
  static Register Get(compiler::AllocatedOperand target) {
    return target.GetRegister();
  }
};
template <>
struct GetRegister<DoubleRegister> {
  static DoubleRegister Get(compiler::AllocatedOperand target) {
    return target.GetDoubleRegister();
  }
};
}  // namespace

void ConstantGapMove::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  switch (node_->opcode()) {
#define CASE(Name)                                \
  case Opcode::k##Name:                           \
    return node_->Cast<Name>()->DoLoadToRegister( \
        masm, GetRegister<Name::OutputRegister>::Get(target()));
    CONSTANT_VALUE_NODE_LIST(CASE)
#undef CASE
    default:
      UNREACHABLE();
  }
}

void GapMove::SetValueLocationConstraints() { UNREACHABLE(); }
void GapMove::GenerateCode(MaglevAssembler* masm,
                           const ProcessingState& state) {
  DCHECK_EQ(source().representation(), target().representation());
  MachineRepresentation repr = source().representation();
  if (source().IsRegister()) {
    Register source_reg = ToRegister(source());
    if (target().IsAnyRegister()) {
      DCHECK(target().IsRegister());
      __ MoveRepr(repr, ToRegister(target()), source_reg);
    } else {
      __ MoveRepr(repr, masm->ToMemOperand(target()), source_reg);
    }
  } else if (source().IsDoubleRegister()) {
    DoubleRegister source_reg = ToDoubleRegister(source());
    if (target().IsAnyRegister()) {
      DCHECK(target().IsDoubleRegister());
      __ Move(ToDoubleRegister(target()), source_reg);
    } else {
      __ StoreFloat64(masm->ToMemOperand(target()), source_reg);
    }
  } else {
    DCHECK(source().IsAnyStackSlot());
    MemOperand source_op = masm->ToMemOperand(source());
    if (target().IsRegister()) {
      __ MoveRepr(repr, ToRegister(target()), source_op);
    } else if (target().IsDoubleRegister()) {
      __ LoadFloat64(ToDoubleRegister(target()), source_op);
    } else {
      DCHECK(target().IsAnyStackSlot());
      __ MoveRepr(repr, masm->ToMemOperand(target()), source_op);
    }
  }
}

void AssertInt32::SetValueLocationConstraints() {
  UseRegister(left_input());
  UseRegister(right_input());
}
void AssertInt32::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  __ CompareInt32(ToRegister(left_input()), ToRegister(right_input()));
  __ Check(ToCondition(condition_), reason_);
}

void CheckUint32IsSmi::SetValueLocationConstraints() { UseRegister(input()); }
void CheckUint32IsSmi::GenerateCode(MaglevAssembler* masm,
                                    const ProcessingState& state) {
  Register reg = ToRegister(input());
  // Perform an unsigned comparison against Smi::kMaxValue.
  __ Cmp(reg, Smi::kMaxValue);
  __ EmitEagerDeoptIf(kUnsignedGreaterThan, DeoptimizeReason::kNotASmi, this);
}

void CheckedSmiUntag::SetValueLocationConstraints() {
  UseRegister(input());
  DefineSameAsFirst(this);
}

void CheckedSmiUntag::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  Register value = ToRegister(input());
  // TODO(leszeks): Consider optimizing away this test and using the carry bit
  // of the `sarl` for cases where the deopt uses the value from a different
  // register.
  Condition is_smi = __ CheckSmi(value);
  __ EmitEagerDeoptIf(NegateCondition(is_smi), DeoptimizeReason::kNotASmi,
                      this);
  __ SmiToInt32(value);
}

void UnsafeSmiUntag::SetValueLocationConstraints() {
  UseRegister(input());
  DefineSameAsFirst(this);
}

void UnsafeSmiUntag::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  Register value = ToRegister(input());
  __ AssertSmi(value);
  __ SmiToInt32(value);
}

void CheckInt32IsSmi::SetValueLocationConstraints() { UseRegister(input()); }
void CheckInt32IsSmi::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  // TODO(leszeks): This basically does a SmiTag and throws the result away.
  // Don't throw the result away if we want to actually use it.
  Register reg = ToRegister(input());
  Label* fail = __ GetDeoptLabel(this, DeoptimizeReason::kNotASmi);
  __ CheckInt32IsSmi(reg, fail);
}

void CheckedInt32ToUint32::SetValueLocationConstraints() {
  UseRegister(input());
  DefineSameAsFirst(this);
}
void CheckedInt32ToUint32::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  __ CompareInt32AndJumpIf(
      ToRegister(input()), 0, kLessThan,
      __ GetDeoptLabel(this, DeoptimizeReason::kNotUint32));
}

void CheckHoleyFloat64IsSmi::SetValueLocationConstraints() {
  UseRegister(input());
  set_temporaries_needed(1);
}
void CheckHoleyFloat64IsSmi::GenerateCode(MaglevAssembler* masm,
                                          const ProcessingState& state) {
  DoubleRegister value = ToDoubleRegister(input());
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();
  Label* fail = __ GetDeoptLabel(this, DeoptimizeReason::kNotASmi);
  __ TryTruncateDoubleToInt32(scratch, value, fail);
  __ CheckInt32IsSmi(scratch, fail, scratch);
}

void CheckedSmiTagInt32::SetValueLocationConstraints() {
  UseAndClobberRegister(input());
  DefineSameAsFirst(this);
}
void CheckedSmiTagInt32::GenerateCode(MaglevAssembler* masm,
                                      const ProcessingState& state) {
  Register reg = ToRegister(input());
  Label* fail = __ GetDeoptLabel(this, DeoptimizeReason::kNotASmi);
  // None of the mutated input registers should be a register input into the
  // eager deopt info.
  DCHECK_REGLIST_EMPTY(RegList{reg} &
                       GetGeneralRegistersUsedAsInputs(eager_deopt_info()));
  __ SmiTagInt32AndJumpIfFail(reg, fail);
}

void CheckedSmiTagUint32::SetValueLocationConstraints() {
  UseRegister(input());
  DefineSameAsFirst(this);
}
void CheckedSmiTagUint32::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  Register reg = ToRegister(input());
  Label* fail = __ GetDeoptLabel(this, DeoptimizeReason::kNotASmi);
  // None of the mutated input registers should be a register input into the
  // eager deopt info.
  DCHECK_REGLIST_EMPTY(RegList{reg} &
                       GetGeneralRegistersUsedAsInputs(eager_deopt_info()));
  __ SmiTagUint32AndJumpIfFail(reg, fail);
}

void UnsafeSmiTag::SetValueLocationConstraints() {
  UseRegister(input());
  DefineSameAsFirst(this);
}
void UnsafeSmiTag::GenerateCode(MaglevAssembler* masm,
                                const ProcessingState& state) {
  Register reg = ToRegister(input());
  switch (input().node()->properties().value_representation()) {
    case ValueRepresentation::kInt32:
      __ UncheckedSmiTagInt32(reg);
      break;
    case ValueRepresentation::kUint32:
      __ UncheckedSmiTagUint32(reg);
      break;
    default:
      UNREACHABLE();
  }
}

namespace {

void JumpToFailIfNotHeapNumberOrOddball(
    MaglevAssembler* masm, Register value,
    TaggedToFloat64ConversionType conversion_type, Label* fail) {
  switch (conversion_type) {
    case TaggedToFloat64ConversionType::kNumberOrOddball:
      // Check if HeapNumber or Oddball, jump to fail otherwise.
      static_assert(InstanceType::HEAP_NUMBER_TYPE + 1 ==
                    InstanceType::ODDBALL_TYPE);
      if (fail) {
        __ CompareObjectTypeRange(value, InstanceType::HEAP_NUMBER_TYPE,
                                  InstanceType::ODDBALL_TYPE);
        __ JumpIf(kUnsignedGreaterThan, fail);
      } else {
        if (v8_flags.debug_code) {
          __ CompareObjectTypeRange(value, InstanceType::HEAP_NUMBER_TYPE,
                                    InstanceType::ODDBALL_TYPE);
          __ Assert(kUnsignedLessThanEqual, AbortReason::kUnexpectedValue);
        }
      }
      break;
    case TaggedToFloat64ConversionType::kOnlyNumber:
      // Check if HeapNumber, jump to fail otherwise.
      if (fail) {
        __ IsObjectType(value, InstanceType::HEAP_NUMBER_TYPE);
        __ JumpIf(kNotEqual, fail);
      } else {
        if (v8_flags.debug_code) {
          __ IsObjectType(value, InstanceType::HEAP_NUMBER_TYPE);
          __ Assert(kEqual, AbortReason::kUnexpectedValue);
        }
      }
      break;
  }
}

void TryUnboxNumberOrOddball(MaglevAssembler* masm, DoubleRegister dst,
                             Register clobbered_src,
                             TaggedToFloat64ConversionType conversion_type,
                             Label* fail) {
  Label is_not_smi, done;
  // Check if Smi.
  __ JumpIfNotSmi(clobbered_src, &is_not_smi, Label::kNear);
  // If Smi, convert to Float64.
  __ SmiToInt32(clobbered_src);
  __ Int32ToDouble(dst, clobbered_src);
  __ Jump(&done, Label::kNear);
  __ bind(&is_not_smi);
  JumpToFailIfNotHeapNumberOrOddball(masm, clobbered_src, conversion_type,
                                     fail);
  static_assert(HeapNumber::kValueOffset == Oddball::kToNumberRawOffset);
  __ LoadHeapNumberValue(dst, clobbered_src);
  __ bind(&done);
}

}  // namespace

void CheckedNumberOrOddballToFloat64::SetValueLocationConstraints() {
  UseAndClobberRegister(input());
  DefineAsRegister(this);
}
void CheckedNumberOrOddballToFloat64::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register value = ToRegister(input());
  TryUnboxNumberOrOddball(
      masm, ToDoubleRegister(result()), value, conversion_type(),
      __ GetDeoptLabel(this, DeoptimizeReason::kNotANumberOrOddball));
}

void UncheckedNumberOrOddballToFloat64::SetValueLocationConstraints() {
  UseAndClobberRegister(input());
  DefineAsRegister(this);
}
void UncheckedNumberOrOddballToFloat64::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register value = ToRegister(input());
  TryUnboxNumberOrOddball(masm, ToDoubleRegister(result()), value,
                          conversion_type(), nullptr);
}

namespace {

void EmitTruncateNumberOrOddballToInt32(
    MaglevAssembler* masm, Register value, Register result_reg,
    TaggedToFloat64ConversionType conversion_type, Label* not_a_number) {
  Label is_not_smi, done;
  // Check if Smi.
  __ JumpIfNotSmi(value, &is_not_smi, Label::kNear);
  // If Smi, convert to Int32.
  __ SmiToInt32(value);
  __ Jump(&done, Label::kNear);
  __ bind(&is_not_smi);
  JumpToFailIfNotHeapNumberOrOddball(masm, value, conversion_type,
                                     not_a_number);
  static_assert(HeapNumber::kValueOffset == Oddball::kToNumberRawOffset);
  MaglevAssembler::ScratchRegisterScope temps(masm);
  DoubleRegister double_value = temps.GetDefaultScratchDoubleRegister();
  __ LoadHeapNumberValue(double_value, value);
  __ TruncateDoubleToInt32(result_reg, double_value);
  __ bind(&done);
}

}  // namespace

void CheckedObjectToIndex::SetValueLocationConstraints() {
  UseRegister(object_input());
  DefineAsRegister(this);
  set_double_temporaries_needed(1);
}
void CheckedObjectToIndex::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register result_reg = ToRegister(result());
  ZoneLabelRef done(masm);
  __ JumpIfNotSmi(
      object,
      __ MakeDeferredCode(
          [](MaglevAssembler* masm, Register object, Register result_reg,
             ZoneLabelRef done, CheckedObjectToIndex* node) {
            MaglevAssembler::ScratchRegisterScope temps(masm);
            Register map = temps.GetDefaultScratchRegister();
            Label check_string;
            __ LoadMap(map, object);
            __ JumpIfNotRoot(map, RootIndex::kHeapNumberMap, &check_string,
                             Label::kNear);
            {
              DoubleRegister number_value = temps.AcquireDouble();
              __ LoadHeapNumberValue(number_value, object);
              __ TryChangeFloat64ToIndex(
                  result_reg, number_value, *done,
                  __ GetDeoptLabel(node, DeoptimizeReason::kNotInt32));
            }
            __ bind(&check_string);
            __ CompareInstanceTypeRange(map, map, FIRST_STRING_TYPE,
                                        LAST_STRING_TYPE);
            // The IC will go generic if it encounters something other than a
            // Number or String key.
            __ EmitEagerDeoptIf(kUnsignedGreaterThan,
                                DeoptimizeReason::kNotInt32, node);
            {
              // TODO(verwaest): Load the cached number from the string hash.
              RegisterSnapshot snapshot = node->register_snapshot();
              snapshot.live_registers.clear(result_reg);
              DCHECK(!snapshot.live_tagged_registers.has(result_reg));
              {
                SaveRegisterStateForCall save_register_state(masm, snapshot);
                AllowExternalCallThatCantCauseGC scope(masm);
                __ PrepareCallCFunction(1);
                __ Move(arg_reg_1, object);
                __ CallCFunction(
                    ExternalReference::string_to_array_index_function(), 1);
                // No need for safepoint since this is a fast C call.
                __ Move(result_reg, kReturnRegister0);
              }
              __ CompareInt32AndJumpIf(result_reg, 0, kGreaterThanEqual, *done);
              __ EmitEagerDeopt(node, DeoptimizeReason::kNotInt32);
            }
          },
          object, result_reg, done, this));

  // If we didn't enter the deferred block, we're a Smi.
  __ SmiToInt32(result_reg, object);
  __ bind(*done);
}

void CheckedTruncateNumberOrOddballToInt32::SetValueLocationConstraints() {
  UseRegister(input());
  DefineSameAsFirst(this);
}
void CheckedTruncateNumberOrOddballToInt32::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register value = ToRegister(input());
  Register result_reg = ToRegister(result());
  DCHECK_EQ(value, result_reg);
  Label* deopt_label =
      __ GetDeoptLabel(this, DeoptimizeReason::kNotANumberOrOddball);
  EmitTruncateNumberOrOddballToInt32(masm, value, result_reg, conversion_type(),
                                     deopt_label);
}

void TruncateNumberOrOddballToInt32::SetValueLocationConstraints() {
  UseRegister(input());
  DefineSameAsFirst(this);
}
void TruncateNumberOrOddballToInt32::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register value = ToRegister(input());
  Register result_reg = ToRegister(result());
  DCHECK_EQ(value, result_reg);
  EmitTruncateNumberOrOddballToInt32(masm, value, result_reg, conversion_type(),
                                     nullptr);
}

void ChangeInt32ToFloat64::SetValueLocationConstraints() {
  UseRegister(input());
  DefineAsRegister(this);
}
void ChangeInt32ToFloat64::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  __ Int32ToDouble(ToDoubleRegister(result()), ToRegister(input()));
}

void ChangeUint32ToFloat64::SetValueLocationConstraints() {
  UseRegister(input());
  DefineAsRegister(this);
}
void ChangeUint32ToFloat64::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  __ Uint32ToDouble(ToDoubleRegister(result()), ToRegister(input()));
}

void CheckMaps::SetValueLocationConstraints() {
  UseRegister(receiver_input());
  set_temporaries_needed(MapCompare::TemporaryCount(maps_.size()));
}

void CheckMaps::GenerateCode(MaglevAssembler* masm,
                             const ProcessingState& state) {
  Register object = ToRegister(receiver_input());

  // We emit an unconditional deopt if we intersect the map sets and the
  // intersection is empty.
  DCHECK(!maps().is_empty());

  bool maps_include_heap_number = AnyMapIsHeapNumber(maps());

  // Exprimentally figured out map limit (with slack) which allows us to use
  // near jumps in the code below
  constexpr int kMapCountForNearJumps = 10;
  Label::Distance jump_distance = maps().size() <= kMapCountForNearJumps
                                      ? Label::Distance::kNear
                                      : Label::Distance::kFar;

  Label done;
  if (check_type() == CheckType::kOmitHeapObjectCheck) {
    __ AssertNotSmi(object);
  } else {
    Condition is_smi = __ CheckSmi(object);
    if (maps_include_heap_number) {
      // Smis count as matching the HeapNumber map, so we're done.
      __ JumpIf(is_smi, &done, jump_distance);
    } else {
      __ EmitEagerDeoptIf(is_smi, DeoptimizeReason::kWrongMap, this);
    }
  }

  MapCompare map_compare(masm, object, maps_.size());
  size_t map_count = maps().size();
  for (size_t i = 0; i < map_count - 1; ++i) {
    Handle<Map> map = maps().at(i).object();
    map_compare.Generate(map);
    __ JumpIf(kEqual, &done, jump_distance);
  }
  Handle<Map> last_map = maps().at(map_count - 1).object();
  map_compare.Generate(last_map);
  __ EmitEagerDeoptIfNotEqual(DeoptimizeReason::kWrongMap, this);
  __ bind(&done);
}

int CheckMapsWithMigration::MaxCallStackArgs() const {
  DCHECK_EQ(Runtime::FunctionForId(Runtime::kTryMigrateInstance)->nargs, 1);
  return 1;
}

void CheckMapsWithMigration::SetValueLocationConstraints() {
  UseRegister(receiver_input());
  set_temporaries_needed(MapCompare::TemporaryCount(maps_.size()));
}

void CheckMapsWithMigration::GenerateCode(MaglevAssembler* masm,
                                          const ProcessingState& state) {
  // We emit an unconditional deopt if we intersect the map sets and the
  // intersection is empty.
  DCHECK(!maps().is_empty());

  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register object = ToRegister(receiver_input());

  bool maps_include_heap_number = AnyMapIsHeapNumber(maps());

  ZoneLabelRef map_checks(masm), done(masm);

  if (check_type() == CheckType::kOmitHeapObjectCheck) {
    __ AssertNotSmi(object);
  } else {
    Condition is_smi = __ CheckSmi(object);
    if (maps_include_heap_number) {
      // Smis count as matching the HeapNumber map, so we're done.
      __ JumpIf(is_smi, *done);
    } else {
      __ EmitEagerDeoptIf(is_smi, DeoptimizeReason::kWrongMap, this);
    }
  }

  // If we jump from here from the deferred code (below), we need to reload
  // the map.
  __ bind(*map_checks);

  RegisterSnapshot save_registers = register_snapshot();
  // Make sure that the object register is not clobbered by the
  // Runtime::kMigrateInstance runtime call. It's ok to clobber the register
  // where the object map is, since the map is reloaded after the runtime call.
  save_registers.live_registers.set(object);
  save_registers.live_tagged_registers.set(object);

  // We can eager deopt after the snapshot, so make sure the nodes used by the
  // deopt are included in it.
  // TODO(leszeks): This is a bit of a footgun -- we likely want the snapshot to
  // always include eager deopt input registers.
  AddDeoptRegistersToSnapshot(&save_registers, eager_deopt_info());

  size_t map_count = maps().size();
  bool has_migration_targets = false;
  MapCompare map_compare(masm, object, maps_.size());
  for (size_t i = 0; i < map_count; ++i) {
    Handle<Map> map_handle = maps().at(i).object();
    map_compare.Generate(map_handle);
    const bool last_map = (i == map_count - 1);
    if (!last_map) {
      __ JumpIf(kEqual, *done);
    }
    if (map_handle->is_migration_target()) {
      has_migration_targets = true;
    }
  }

  if (!has_migration_targets) {
    // Emit deopt for the last map.
    __ EmitEagerDeoptIf(kNotEqual, DeoptimizeReason::kWrongMap, this);
  } else {
    __ JumpToDeferredIf(
        kNotEqual,
        [](MaglevAssembler* masm, RegisterSnapshot register_snapshot,
           ZoneLabelRef map_checks, MapCompare map_compare,
           CheckMapsWithMigration* node) {
          Label* deopt = __ GetDeoptLabel(node, DeoptimizeReason::kWrongMap);
          // If the map is not deprecated, we fail the map check.
          __ TestInt32AndJumpIfAllClear(
              FieldMemOperand(map_compare.GetMap(), Map::kBitField3Offset),
              Map::Bits3::IsDeprecatedBit::kMask, deopt);

          // Otherwise, try migrating the object.
          Register return_val = Register::no_reg();
          {
            SaveRegisterStateForCall save_register_state(masm,
                                                         register_snapshot);

            __ Push(map_compare.GetObject());
            __ Move(kContextRegister, masm->native_context().object());
            __ CallRuntime(Runtime::kTryMigrateInstance);
            save_register_state.DefineSafepoint();

            // Make sure the return value is preserved across the live
            // register restoring pop all.
            return_val = kReturnRegister0;
            MaglevAssembler::ScratchRegisterScope temps(masm);
            Register scratch = temps.GetDefaultScratchRegister();
            if (register_snapshot.live_registers.has(return_val)) {
              DCHECK(!register_snapshot.live_registers.has(scratch));
              __ Move(scratch, return_val);
              return_val = scratch;
            }
          }

          // On failure, the returned value is Smi zero.
          __ CompareTaggedAndJumpIf(return_val, Smi::zero(), kEqual, deopt);

          // Otherwise, the return value is the object (it's always the same
          // object we called TryMigrate with). We already have it in a
          // register, so we can ignore the return value. We'll need to reload
          // the map though since it might have changed; it's done right after
          // the map_checks label.
          __ Jump(*map_checks);
        },
        save_registers, map_checks, map_compare, this);
    // If the jump to deferred code was not taken, the map was equal to the
    // last map.
    __ Jump(*done);
  }  // End of the `has_migration_targets` case.
  __ bind(*done);
}

int DeleteProperty::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kDeleteProperty>::type;
  return D::GetStackParameterCount();
}
void DeleteProperty::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kDeleteProperty>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object(), D::GetRegisterParameter(D::kObject));
  UseFixed(key(), D::GetRegisterParameter(D::kKey));
  DefineAsFixed(this, kReturnRegister0);
}
void DeleteProperty::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  __ CallBuiltin<Builtin::kDeleteProperty>(
      context(),                              // context
      object(),                               // object
      key(),                                  // key
      Smi::FromInt(static_cast<int>(mode()))  // language mode
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int ForInPrepare::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kForInPrepare>::type;
  return D::GetStackParameterCount();
}
void ForInPrepare::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kForInPrepare>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(enumerator(), D::GetRegisterParameter(D::kEnumerator));
  DefineAsFixed(this, kReturnRegister0);
}
void ForInPrepare::GenerateCode(MaglevAssembler* masm,
                                const ProcessingState& state) {
  __ CallBuiltin<Builtin::kForInPrepare>(
      context(),                                    // context
      enumerator(),                                 // enumerator
      TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
      feedback().vector                             // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int ForInNext::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kForInNext>::type;
  return D::GetStackParameterCount();
}
void ForInNext::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kForInNext>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(receiver(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(cache_array(), D::GetRegisterParameter(D::kCacheArray));
  UseFixed(cache_type(), D::GetRegisterParameter(D::kCacheType));
  UseFixed(cache_index(), D::GetRegisterParameter(D::kCacheIndex));
  DefineAsFixed(this, kReturnRegister0);
}
void ForInNext::GenerateCode(MaglevAssembler* masm,
                             const ProcessingState& state) {
  __ CallBuiltin<Builtin::kForInNext>(context(),           // context
                                      feedback().index(),  // feedback slot
                                      receiver(),          // receiver
                                      cache_array(),       // cache array
                                      cache_type(),        // cache type
                                      cache_index(),       // cache index
                                      feedback().vector    // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int GetIterator::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kGetIteratorWithFeedback>::type;
  return D::GetStackParameterCount();
}
void GetIterator::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kGetIteratorWithFeedback>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(receiver(), D::GetRegisterParameter(D::kReceiver));
  DefineAsFixed(this, kReturnRegister0);
}
void GetIterator::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  __ CallBuiltin<Builtin::kGetIteratorWithFeedback>(
      context(),                             // context
      receiver(),                            // receiver
      TaggedIndex::FromIntptr(load_slot()),  // feedback load slot
      TaggedIndex::FromIntptr(call_slot()),  // feedback call slot
      feedback()                             // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

template <class Derived, Operation kOperation>
void Int32CompareNode<Derived, kOperation>::SetValueLocationConstraints() {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineAsRegister(this);
}

template <class Derived, Operation kOperation>
void Int32CompareNode<Derived, kOperation>::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register result = ToRegister(this->result());
  Label is_true, end;
  __ CompareInt32AndJumpIf(ToRegister(left_input()), ToRegister(right_input()),
                           ConditionFor(kOperation), &is_true,
                           Label::Distance::kNear);
  // TODO(leszeks): Investigate loading existing materialisations of roots here,
  // if available.
  __ LoadRoot(result, RootIndex::kFalseValue);
  __ jmp(&end);
  {
    __ bind(&is_true);
    __ LoadRoot(result, RootIndex::kTrueValue);
  }
  __ bind(&end);
}

#define DEF_OPERATION(Name)                               \
  void Name::SetValueLocationConstraints() {              \
    Base::SetValueLocationConstraints();                  \
  }                                                       \
  void Name::GenerateCode(MaglevAssembler* masm,          \
                          const ProcessingState& state) { \
    Base::GenerateCode(masm, state);                      \
  }
DEF_OPERATION(Int32Equal)
DEF_OPERATION(Int32StrictEqual)
DEF_OPERATION(Int32LessThan)
DEF_OPERATION(Int32LessThanOrEqual)
DEF_OPERATION(Int32GreaterThan)
DEF_OPERATION(Int32GreaterThanOrEqual)
#undef DEF_OPERATION

template <class Derived, Operation kOperation>
void Float64CompareNode<Derived, kOperation>::SetValueLocationConstraints() {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineAsRegister(this);
}

template <class Derived, Operation kOperation>
void Float64CompareNode<Derived, kOperation>::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  DoubleRegister left = ToDoubleRegister(left_input());
  DoubleRegister right = ToDoubleRegister(right_input());
  Register result = ToRegister(this->result());
  Label is_false, end;
  __ CompareFloat64(left, right);
  // Ucomisd sets these flags accordingly:
  //   UNORDERED(one of the operands is a NaN): ZF,PF,CF := 111;
  //   GREATER_THAN: ZF,PF,CF := 000;
  //   LESS_THAN: ZF,PF,CF := 001;
  //   EQUAL: ZF,PF,CF := 100;
  // Since ZF can be set by NaN or EQUAL, we check for NaN first.
  __ JumpIf(ConditionForNaN(), &is_false);
  __ JumpIf(NegateCondition(ConditionForFloat64(kOperation)), &is_false);
  // TODO(leszeks): Investigate loading existing materialisations of roots here,
  // if available.
  __ LoadRoot(result, RootIndex::kTrueValue);
  __ Jump(&end);
  {
    __ bind(&is_false);
    __ LoadRoot(result, RootIndex::kFalseValue);
  }
  __ bind(&end);
}

#define DEF_OPERATION(Name)                               \
  void Name::SetValueLocationConstraints() {              \
    Base::SetValueLocationConstraints();                  \
  }                                                       \
  void Name::GenerateCode(MaglevAssembler* masm,          \
                          const ProcessingState& state) { \
    Base::GenerateCode(masm, state);                      \
  }
DEF_OPERATION(Float64Equal)
DEF_OPERATION(Float64StrictEqual)
DEF_OPERATION(Float64LessThan)
DEF_OPERATION(Float64LessThanOrEqual)
DEF_OPERATION(Float64GreaterThan)
DEF_OPERATION(Float64GreaterThanOrEqual)
#undef DEF_OPERATION

void CheckedHoleyFloat64ToFloat64::SetValueLocationConstraints() {
  UseRegister(input());
  DefineSameAsFirst(this);
  set_temporaries_needed(1);
}
void CheckedHoleyFloat64ToFloat64::GenerateCode(MaglevAssembler* masm,
                                                const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  __ JumpIfHoleNan(ToDoubleRegister(input()), temps.Acquire(),
                   __ GetDeoptLabel(this, DeoptimizeReason::kHole));
}

void LoadDoubleField::SetValueLocationConstraints() {
  UseRegister(object_input());
  DefineAsRegister(this);
  set_temporaries_needed(1);
}
void LoadDoubleField::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register tmp = temps.Acquire();
  Register object = ToRegister(object_input());
  __ AssertNotSmi(object);
  __ DecompressTagged(tmp, FieldMemOperand(object, offset()));
  __ AssertNotSmi(tmp);
  __ LoadHeapNumberValue(ToDoubleRegister(result()), tmp);
}

void LoadTaggedField::SetValueLocationConstraints() {
  UseRegister(object_input());
  DefineAsRegister(this);
}
void LoadTaggedField::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  Register object = ToRegister(object_input());
  __ AssertNotSmi(object);
  if (this->decompresses_tagged_result()) {
    __ LoadTaggedField(ToRegister(result()), object, offset());
  } else {
    __ LoadTaggedFieldWithoutDecompressing(ToRegister(result()), object,
                                           offset());
  }
}

void LoadTaggedFieldByFieldIndex::SetValueLocationConstraints() {
  UseRegister(object_input());
  UseAndClobberRegister(index_input());
  DefineAsRegister(this);
  set_temporaries_needed(1);
  set_double_temporaries_needed(1);
}
void LoadTaggedFieldByFieldIndex::GenerateCode(MaglevAssembler* masm,
                                               const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register index = ToRegister(index_input());
  Register result_reg = ToRegister(result());
  __ AssertNotSmi(object);
  __ AssertSmi(index);

  ZoneLabelRef done(masm);

  // For in-object properties, the index is encoded as:
  //
  //      index = actual_index | is_double_bit | smi_tag_bit
  //            = actual_index << 2 | is_double_bit << 1
  //
  // The value we want is at the field offset:
  //
  //      (actual_index << kTaggedSizeLog2) + JSObject::kHeaderSize
  //
  // We could get index from actual_index by shifting away the double and smi
  // bits. But, note that `kTaggedSizeLog2 == 2` and `index` encodes
  // `actual_index` with a two bit shift. So, we can do some rearranging
  // to get the offset without shifting:
  //
  //      ((index >> 2) << kTaggedSizeLog2 + JSObject::kHeaderSize
  //
  //    [Expand definitions of index and kTaggedSizeLog2]
  //    = (((actual_index << 2 | is_double_bit << 1) >> 2) << 2)
  //           + JSObject::kHeaderSize
  //
  //    [Cancel out shift down and shift up, clear is_double bit by subtracting]
  //    = (actual_index << 2 | is_double_bit << 1) - (is_double_bit << 1)
  //           + JSObject::kHeaderSize
  //
  //    [Fold together the constants, and collapse definition of index]
  //    = index + (JSObject::kHeaderSize - (is_double_bit << 1))
  //
  //
  // For out-of-object properties, the encoding is:
  //
  //     index = (-1 - actual_index) | is_double_bit | smi_tag_bit
  //           = (-1 - actual_index) << 2 | is_double_bit << 1
  //           = (-1 - actual_index) * 4 + (is_double_bit ? 2 : 0)
  //           = -(actual_index * 4) + (is_double_bit ? 2 : 0) - 4
  //           = -(actual_index << 2) + (is_double_bit ? 2 : 0) - 4
  //
  // The value we want is in the property array at offset:
  //
  //      (actual_index << kTaggedSizeLog2) + FixedArray::kHeaderSize
  //
  //    [Expand definition of kTaggedSizeLog2]
  //    = (actual_index << 2) + FixedArray::kHeaderSize
  //
  //    [Substitute in index]
  //    = (-index + (is_double_bit ? 2 : 0) - 4) + FixedArray::kHeaderSize
  //
  //    [Fold together the constants]
  //    = -index + (FixedArray::kHeaderSize + (is_double_bit ? 2 : 0) - 4))
  //
  // This allows us to simply negate the index register and do a load with
  // otherwise constant offset.

  // Check if field is a mutable double field.
  static constexpr int32_t kIsDoubleBitMask = 1 << kSmiTagSize;
  __ TestInt32AndJumpIfAnySet(
      index, kIsDoubleBitMask,
      __ MakeDeferredCode(
          [](MaglevAssembler* masm, Register object, Register index,
             Register result_reg, RegisterSnapshot register_snapshot,
             ZoneLabelRef done) {
            // The field is a Double field, a.k.a. a mutable HeapNumber.
            static const int kIsDoubleBit = 1;

            // Check if field is in-object or out-of-object. The is_double bit
            // value doesn't matter, since negative values will stay negative.
            Label if_outofobject, loaded_field;
            __ CompareInt32AndJumpIf(index, 0, kLessThan, &if_outofobject);

            // The field is located in the {object} itself.
            {
              // See giant comment above.
              static_assert(kTaggedSizeLog2 == 2);
              static_assert(kSmiTagSize == 1);
              // We haven't untagged, so we need to sign extend.
              __ SignExtend32To64Bits(index, index);
              __ LoadTaggedFieldByIndex(
                  result_reg, object, index, 1,
                  JSObject::kHeaderSize - (kIsDoubleBit << kSmiTagSize));
              __ Jump(&loaded_field);
            }

            __ bind(&if_outofobject);
            {
              MaglevAssembler::ScratchRegisterScope temps(masm);
              Register property_array = temps.Acquire();
              // Load the property array.
              __ LoadTaggedField(
                  property_array,
                  FieldMemOperand(object, JSObject::kPropertiesOrHashOffset));

              // See giant comment above.
              static_assert(kSmiTagSize == 1);
              __ NegateInt32(index);
              __ LoadTaggedFieldByIndex(
                  result_reg, property_array, index, 1,
                  FixedArray::kHeaderSize + (kIsDoubleBit << kSmiTagSize) - 4);
              __ Jump(&loaded_field);
            }

            __ bind(&loaded_field);
            // We may have transitioned in-place away from double, so check that
            // this is a HeapNumber -- otherwise the load is fine and we don't
            // need to copy anything anyway.
            __ JumpIfSmi(result_reg, *done);
            MaglevAssembler::ScratchRegisterScope temps(masm);
            Register map = temps.Acquire();
            // Hack: The temporary allocated for `map` might alias the result
            // register. If it does, use the index register as a temporary
            // instead (since it's clobbered anyway).
            // TODO(leszeks): Extend the result register's lifetime to overlap
            // the temporaries, so that this alias isn't possible.
            if (map == result_reg) {
              DCHECK_NE(map, index);
              map = index;
            }
            __ LoadMap(map, result_reg);
            __ JumpIfNotRoot(map, RootIndex::kHeapNumberMap, *done);
            DoubleRegister double_value = temps.AcquireDouble();
            __ LoadHeapNumberValue(double_value, result_reg);
            __ AllocateHeapNumber(register_snapshot, result_reg, double_value);
            __ Jump(*done);
          },
          object, index, result_reg, register_snapshot(), done));

  // The field is a proper Tagged field on {object}. The {index} is shifted
  // to the left by one in the code below.
  {
    static const int kIsDoubleBit = 0;

    // Check if field is in-object or out-of-object. The is_double bit value
    // doesn't matter, since negative values will stay negative.
    Label if_outofobject;
    __ CompareInt32AndJumpIf(index, 0, kLessThan, &if_outofobject);

    // The field is located in the {object} itself.
    {
      // See giant comment above.
      static_assert(kTaggedSizeLog2 == 2);
      static_assert(kSmiTagSize == 1);
      // We haven't untagged, so we need to sign extend.
      __ SignExtend32To64Bits(index, index);
      __ LoadTaggedFieldByIndex(
          result_reg, object, index, 1,
          JSObject::kHeaderSize - (kIsDoubleBit << kSmiTagSize));
      __ Jump(*done);
    }

    __ bind(&if_outofobject);
    {
      MaglevAssembler::ScratchRegisterScope temps(masm);
      Register property_array = temps.Acquire();
      // Load the property array.
      __ LoadTaggedField(
          property_array,
          FieldMemOperand(object, JSObject::kPropertiesOrHashOffset));

      // See giant comment above.
      static_assert(kSmiTagSize == 1);
      __ NegateInt32(index);
      __ LoadTaggedFieldByIndex(
          result_reg, property_array, index, 1,
          FixedArray::kHeaderSize + (kIsDoubleBit << kSmiTagSize) - 4);
      // Fallthrough to `done`.
    }
  }

  __ bind(*done);
}

void LoadFixedArrayElement::SetValueLocationConstraints() {
  UseRegister(elements_input());
  UseRegister(index_input());
  DefineAsRegister(this);
}
void LoadFixedArrayElement::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  Register elements = ToRegister(elements_input());
  Register index = ToRegister(index_input());
  Register result_reg = ToRegister(result());
  if (this->decompresses_tagged_result()) {
    __ LoadFixedArrayElement(result_reg, elements, index);
  } else {
    __ LoadFixedArrayElementWithoutDecompressing(result_reg, elements, index);
  }
}

void LoadFixedDoubleArrayElement::SetValueLocationConstraints() {
  UseRegister(elements_input());
  UseRegister(index_input());
  DefineAsRegister(this);
}
void LoadFixedDoubleArrayElement::GenerateCode(MaglevAssembler* masm,
                                               const ProcessingState& state) {
  Register elements = ToRegister(elements_input());
  Register index = ToRegister(index_input());
  DoubleRegister result_reg = ToDoubleRegister(result());
  __ LoadFixedDoubleArrayElement(result_reg, elements, index);
}

void LoadHoleyFixedDoubleArrayElement::SetValueLocationConstraints() {
  UseRegister(elements_input());
  UseRegister(index_input());
  DefineAsRegister(this);
}
void LoadHoleyFixedDoubleArrayElement::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register elements = ToRegister(elements_input());
  Register index = ToRegister(index_input());
  DoubleRegister result_reg = ToDoubleRegister(result());
  __ LoadFixedDoubleArrayElement(result_reg, elements, index);
}

int StoreMap::MaxCallStackArgs() const {
  return WriteBarrierDescriptor::GetStackParameterCount();
}
void StoreMap::SetValueLocationConstraints() {
  UseFixed(object_input(), WriteBarrierDescriptor::ObjectRegister());
  set_temporaries_needed(1);
}
void StoreMap::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  // TODO(leszeks): Consider making this an arbitrary register and push/popping
  // in the deferred path.
  Register object = WriteBarrierDescriptor::ObjectRegister();
  DCHECK_EQ(object, ToRegister(object_input()));
  Register value = temps.Acquire();
  __ Move(value, map_.object());

  __ StoreTaggedFieldWithWriteBarrier(object, HeapObject::kMapOffset, value,
                                      register_snapshot(),
                                      MaglevAssembler::kValueIsDecompressed,
                                      MaglevAssembler::kValueCannotBeSmi);
}

int StoreTaggedFieldWithWriteBarrier::MaxCallStackArgs() const {
  return WriteBarrierDescriptor::GetStackParameterCount();
}
void StoreTaggedFieldWithWriteBarrier::SetValueLocationConstraints() {
  UseFixed(object_input(), WriteBarrierDescriptor::ObjectRegister());
  UseRegister(value_input());
}
void StoreTaggedFieldWithWriteBarrier::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  // TODO(leszeks): Consider making this an arbitrary register and push/popping
  // in the deferred path.
  Register object = WriteBarrierDescriptor::ObjectRegister();
  DCHECK_EQ(object, ToRegister(object_input()));
  Register value = ToRegister(value_input());

  __ StoreTaggedFieldWithWriteBarrier(
      object, offset(), value, register_snapshot(),
      value_input().node()->decompresses_tagged_result()
          ? MaglevAssembler::kValueIsDecompressed
          : MaglevAssembler::kValueIsCompressed,
      MaglevAssembler::kValueCanBeSmi);
}

namespace {

template <typename NodeT, typename Function, typename... Args>
void EmitPolymorphicAccesses(MaglevAssembler* masm, NodeT* node,
                             Register object, Function&& f, Args&&... args) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register object_map = temps.Acquire();
  Label done;
  Label is_number;

  Condition is_smi = __ CheckSmi(object);
  __ JumpIf(is_smi, &is_number);
  __ LoadMap(object_map, object);

  for (const PolymorphicAccessInfo& access_info : node->access_infos()) {
    Label next;
    Label map_found;
    auto& maps = access_info.maps();

    bool has_number_map = false;
    if (HasOnlyStringMaps(base::VectorOf(maps))) {
      MaglevAssembler::ScratchRegisterScope temps(masm);
      Register scratch = temps.GetDefaultScratchRegister();
      __ CompareInstanceTypeRange(object_map, scratch, FIRST_STRING_TYPE,
                                  LAST_STRING_TYPE);
      __ JumpIf(kUnsignedGreaterThan, &next);
      // Fallthrough... to map_found.
    } else {
      for (auto it = maps.begin(); it != maps.end(); ++it) {
        if (it->object()->IsHeapNumberMap()) {
          __ CompareRoot(object_map, RootIndex::kHeapNumberMap);
          has_number_map = true;
        } else {
          __ CompareTagged(object_map, it->object());
        }
        if (it == maps.end() - 1) {
          __ JumpIf(kNotEqual, &next);
          // Fallthrough... to map_found.
        } else {
          __ JumpIf(kEqual, &map_found);
        }
      }
    }

    if (has_number_map) {
      DCHECK(!is_number.is_bound());
      __ bind(&is_number);
    }
    __ bind(&map_found);
    f(masm, node, access_info, object, object_map, std::forward<Args>(args)...);
    __ Jump(&done);

    __ bind(&next);
  }

  // A HeapNumberMap was not found, we should eager deopt here in case of a
  // number.
  if (!is_number.is_bound()) {
    __ bind(&is_number);
  }

  // No map matched!
  __ EmitEagerDeopt(node, DeoptimizeReason::kWrongMap);
  __ bind(&done);
}

}  // namespace

void LoadPolymorphicTaggedField::SetValueLocationConstraints() {
  UseRegister(object_input());
  DefineAsRegister(this);
  set_temporaries_needed(2);
  set_double_temporaries_needed(1);
}
void LoadPolymorphicTaggedField::GenerateCode(MaglevAssembler* masm,
                                              const ProcessingState& state) {
  Register object = ToRegister(object_input());
  EmitPolymorphicAccesses(
      masm, this, object,
      [](MaglevAssembler* masm, LoadPolymorphicTaggedField* node,
         const PolymorphicAccessInfo& access_info, Register object,
         Register map, Register result) {
        switch (access_info.kind()) {
          case PolymorphicAccessInfo::kNotFound:
            __ LoadRoot(result, RootIndex::kUndefinedValue);
            break;
          case PolymorphicAccessInfo::kConstant: {
            Handle<Object> constant = access_info.constant();
            if (constant->IsSmi()) {
              __ Move(result, Smi::cast(*constant));
            } else {
              DCHECK(access_info.constant()->IsHeapObject());
              __ Move(result, Handle<HeapObject>::cast(constant));
            }
            break;
          }
          case PolymorphicAccessInfo::kModuleExport: {
            Register cell = map;  // Reuse scratch.
            __ Move(cell, access_info.cell());
            __ AssertNotSmi(cell);
            __ DecompressTagged(result,
                                FieldMemOperand(cell, Cell::kValueOffset));
            break;
          }
          case PolymorphicAccessInfo::kDataLoad: {
            MaglevAssembler::ScratchRegisterScope temps(masm);
            DoubleRegister double_scratch = temps.AcquireDouble();
            __ LoadDataField(access_info, result, object, map);
            if (access_info.field_index().is_double()) {
              __ LoadHeapNumberValue(double_scratch, result);
              __ AllocateHeapNumber(node->register_snapshot(), result,
                                    double_scratch);
            }
            break;
          }
          case PolymorphicAccessInfo::kStringLength:
            __ StringLength(result, object);
            __ SmiTag(result);
            break;
        }
      },
      ToRegister(result()));
}

void LoadPolymorphicDoubleField::SetValueLocationConstraints() {
  UseRegister(object_input());
  DefineAsRegister(this);
  set_temporaries_needed(1);
}
void LoadPolymorphicDoubleField::GenerateCode(MaglevAssembler* masm,
                                              const ProcessingState& state) {
  Register object = ToRegister(object_input());
  EmitPolymorphicAccesses(
      masm, this, object,
      [](MaglevAssembler* masm, LoadPolymorphicDoubleField* node,
         const PolymorphicAccessInfo& access_info, Register object,
         Register map, DoubleRegister result) {
        Register scratch = map;
        switch (access_info.kind()) {
          case PolymorphicAccessInfo::kDataLoad:
            __ LoadDataField(access_info, scratch, object, map);
            switch (access_info.field_representation().kind()) {
              case Representation::kSmi:
                __ SmiToDouble(result, scratch);
                break;
              case Representation::kDouble:
                __ LoadHeapNumberValue(result, scratch);
                break;
              default:
                UNREACHABLE();
            }
            break;
          case PolymorphicAccessInfo::kConstant: {
            Handle<Object> constant = access_info.constant();
            if (constant->IsSmi()) {
              __ Move(scratch, Smi::cast(*constant));
              __ SmiToDouble(result, scratch);
            } else {
              DCHECK(constant->IsHeapNumber());
              __ Move(result, Handle<HeapNumber>::cast(constant)->value());
            }
            break;
          }
          case PolymorphicAccessInfo::kStringLength:
            __ StringLength(scratch, object);
            __ Int32ToDouble(result, scratch);
            break;
          default:
            UNREACHABLE();
        }
      },
      ToDoubleRegister(result()));
}

void LoadEnumCacheLength::SetValueLocationConstraints() {
  UseRegister(map_input());
  DefineAsRegister(this);
}
void LoadEnumCacheLength::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  Register map = ToRegister(map_input());
  Register result_reg = ToRegister(result());
  __ AssertMap(map);
  __ LoadBitField<Map::Bits3::EnumLengthBits>(
      result_reg, FieldMemOperand(map, Map::kBitField3Offset));
}

int LoadGlobal::MaxCallStackArgs() const {
  if (typeof_mode() == TypeofMode::kNotInside) {
    using D = CallInterfaceDescriptorFor<Builtin::kLoadGlobalIC>::type;
    return D::GetStackParameterCount();
  } else {
    using D =
        CallInterfaceDescriptorFor<Builtin::kLoadGlobalICInsideTypeof>::type;
    return D::GetStackParameterCount();
  }
}
void LoadGlobal::SetValueLocationConstraints() {
  UseFixed(context(), kContextRegister);
  DefineAsFixed(this, kReturnRegister0);
}
void LoadGlobal::GenerateCode(MaglevAssembler* masm,
                              const ProcessingState& state) {
  if (typeof_mode() == TypeofMode::kNotInside) {
    __ CallBuiltin<Builtin::kLoadGlobalIC>(
        context(),                                    // context
        name().object(),                              // name
        TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
        feedback().vector                             // feedback vector
    );
  } else {
    DCHECK_EQ(typeof_mode(), TypeofMode::kInside);
    __ CallBuiltin<Builtin::kLoadGlobalICInsideTypeof>(
        context(),                                    // context
        name().object(),                              // name
        TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
        feedback().vector                             // feedback vector
    );
  }

  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int StoreGlobal::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kStoreGlobalIC>::type;
  return D::GetStackParameterCount();
}
void StoreGlobal::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kStoreGlobalIC>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(value(), D::GetRegisterParameter(D::kValue));
  DefineAsFixed(this, kReturnRegister0);
}
void StoreGlobal::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  __ CallBuiltin<Builtin::kStoreGlobalIC>(
      context(),                                    // context
      name().object(),                              // name
      value(),                                      // value
      TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
      feedback().vector                             // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void CheckValue::SetValueLocationConstraints() { UseRegister(target_input()); }
void CheckValue::GenerateCode(MaglevAssembler* masm,
                              const ProcessingState& state) {
  Register target = ToRegister(target_input());
  __ CompareTagged(target, value().object());
  __ EmitEagerDeoptIfNotEqual(DeoptimizeReason::kWrongValue, this);
}

void CheckValueEqualsInt32::SetValueLocationConstraints() {
  UseRegister(target_input());
}
void CheckValueEqualsInt32::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  Register target = ToRegister(target_input());
  __ CompareInt32(target, value());
  __ EmitEagerDeoptIfNotEqual(DeoptimizeReason::kWrongValue, this);
}

void CheckValueEqualsFloat64::SetValueLocationConstraints() {
  UseRegister(target_input());
  set_double_temporaries_needed(1);
}
void CheckValueEqualsFloat64::GenerateCode(MaglevAssembler* masm,
                                           const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  DoubleRegister scratch = temps.AcquireDouble();
  DoubleRegister target = ToDoubleRegister(target_input());
  __ Move(scratch, value());
  __ CompareFloat64(scratch, target);
  __ EmitEagerDeoptIf(ConditionForNaN(), DeoptimizeReason::kWrongValue, this);
  __ EmitEagerDeoptIfNotEqual(DeoptimizeReason::kWrongValue, this);
}

void CheckValueEqualsString::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kStringEqual>::type;
  UseFixed(target_input(), D::GetRegisterParameter(D::kLeft));
  RequireSpecificTemporary(D::GetRegisterParameter(D::kLength));
}
void CheckValueEqualsString::GenerateCode(MaglevAssembler* masm,
                                          const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<Builtin::kStringEqual>::type;

  ZoneLabelRef end(masm);
  DCHECK_EQ(D::GetRegisterParameter(D::kLeft), ToRegister(target_input()));
  Register target = D::GetRegisterParameter(D::kLeft);
  // Maybe the string is internalized already, do a fast reference check first.
  __ CompareTagged(target, value().object());
  __ JumpIf(kEqual, *end, Label::kNear);

  __ EmitEagerDeoptIf(__ CheckSmi(target), DeoptimizeReason::kWrongValue, this);
  __ CompareObjectTypeRange(target, FIRST_STRING_TYPE, LAST_STRING_TYPE);

  __ JumpToDeferredIf(
      kUnsignedLessThanEqual,
      [](MaglevAssembler* masm, CheckValueEqualsString* node,
         ZoneLabelRef end) {
        Register target = D::GetRegisterParameter(D::kLeft);
        Register string_length = D::GetRegisterParameter(D::kLength);
        __ StringLength(string_length, target);
        __ CompareInt32(string_length, node->value().length());
        __ EmitEagerDeoptIf(kNotEqual, DeoptimizeReason::kWrongValue, node);
        RegisterSnapshot snapshot = node->register_snapshot();
        AddDeoptRegistersToSnapshot(&snapshot, node->eager_deopt_info());
        {
          SaveRegisterStateForCall save_register_state(masm, snapshot);
          __ CallBuiltin<Builtin::kStringEqual>(
              node->target_input(),    // left
              node->value().object(),  // right
              string_length            // length
          );
          save_register_state.DefineSafepoint();
          // Compare before restoring registers, so that the deopt below has the
          // correct register set.
          __ CompareRoot(kReturnRegister0, RootIndex::kTrueValue);
        }
        __ JumpIf(kEqual, *end);
        __ EmitEagerDeopt(node, DeoptimizeReason::kWrongValue);
      },
      this, end);

  __ EmitEagerDeopt(this, DeoptimizeReason::kWrongValue);

  __ bind(*end);
}

void CheckDynamicValue::SetValueLocationConstraints() {
  UseRegister(first_input());
  UseRegister(second_input());
}
void CheckDynamicValue::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  Register first = ToRegister(first_input());
  Register second = ToRegister(second_input());
  __ CompareTagged(first, second);
  __ EmitEagerDeoptIfNotEqual(DeoptimizeReason::kWrongValue, this);
}

void CheckSmi::SetValueLocationConstraints() { UseRegister(receiver_input()); }
void CheckSmi::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  Condition is_smi = __ CheckSmi(object);
  __ EmitEagerDeoptIf(NegateCondition(is_smi), DeoptimizeReason::kNotASmi,
                      this);
}

void CheckHeapObject::SetValueLocationConstraints() {
  UseRegister(receiver_input());
}
void CheckHeapObject::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  Condition is_smi = __ CheckSmi(object);
  __ EmitEagerDeoptIf(is_smi, DeoptimizeReason::kSmi, this);
}

void CheckSymbol::SetValueLocationConstraints() {
  UseRegister(receiver_input());
}
void CheckSymbol::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  if (check_type() == CheckType::kOmitHeapObjectCheck) {
    __ AssertNotSmi(object);
  } else {
    Condition is_smi = __ CheckSmi(object);
    __ EmitEagerDeoptIf(is_smi, DeoptimizeReason::kNotASymbol, this);
  }
  __ IsObjectType(object, SYMBOL_TYPE);
  __ EmitEagerDeoptIf(kNotEqual, DeoptimizeReason::kNotASymbol, this);
}

void CheckInstanceType::SetValueLocationConstraints() {
  UseRegister(receiver_input());
  if (first_instance_type_ != last_instance_type_) {
    set_temporaries_needed(1);
  }
}
void CheckInstanceType::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  if (check_type() == CheckType::kOmitHeapObjectCheck) {
    __ AssertNotSmi(object);
  } else {
    Condition is_smi = __ CheckSmi(object);
    __ EmitEagerDeoptIf(is_smi, DeoptimizeReason::kWrongInstanceType, this);
  }
  if (first_instance_type_ == last_instance_type_) {
    __ IsObjectType(object, first_instance_type_);
    __ EmitEagerDeoptIf(kNotEqual, DeoptimizeReason::kWrongInstanceType, this);
  } else {
    MaglevAssembler::ScratchRegisterScope temps(masm);
    Register map = temps.Acquire();
    __ LoadMap(map, object);
    __ CompareInstanceTypeRange(map, map, first_instance_type_,
                                last_instance_type_);
    __ EmitEagerDeoptIf(kUnsignedGreaterThan,
                        DeoptimizeReason::kWrongInstanceType, this);
  }
}

void CheckFixedArrayNonEmpty::SetValueLocationConstraints() {
  UseRegister(receiver_input());
  set_temporaries_needed(1);
}
void CheckFixedArrayNonEmpty::GenerateCode(MaglevAssembler* masm,
                                           const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  __ AssertNotSmi(object);

  if (v8_flags.debug_code) {
    Label ok;
    __ IsObjectType(object, FIXED_ARRAY_TYPE);
    __ JumpIf(kEqual, &ok);
    __ IsObjectType(object, FIXED_DOUBLE_ARRAY_TYPE);
    __ Assert(kEqual, AbortReason::kOperandIsNotAFixedArray);
    __ bind(&ok);
  }
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register length = temps.Acquire();
  __ LoadTaggedSignedField(length, object, FixedArrayBase::kLengthOffset);
  __ CompareSmiAndJumpIf(
      length, Smi::zero(), kEqual,
      __ GetDeoptLabel(this, DeoptimizeReason::kWrongEnumIndices));
}

void CheckInt32Condition::SetValueLocationConstraints() {
  UseRegister(left_input());
  UseRegister(right_input());
}
void CheckInt32Condition::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  __ CompareInt32(ToRegister(left_input()), ToRegister(right_input()));
  __ EmitEagerDeoptIf(NegateCondition(ToCondition(condition())), reason(),
                      this);
}

void CheckString::SetValueLocationConstraints() {
  UseRegister(receiver_input());
}
void CheckString::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  if (check_type() == CheckType::kOmitHeapObjectCheck) {
    __ AssertNotSmi(object);
  } else {
    Condition is_smi = __ CheckSmi(object);
    __ EmitEagerDeoptIf(is_smi, DeoptimizeReason::kNotAString, this);
  }
  __ CompareObjectTypeRange(object, FIRST_STRING_TYPE, LAST_STRING_TYPE);
  __ EmitEagerDeoptIf(kUnsignedGreaterThan, DeoptimizeReason::kNotAString,
                      this);
}

void ConvertHoleToUndefined::SetValueLocationConstraints() {
  UseRegister(object_input());
  DefineSameAsFirst(this);
}
void ConvertHoleToUndefined::GenerateCode(MaglevAssembler* masm,
                                          const ProcessingState& state) {
  Label done;
  DCHECK_EQ(ToRegister(object_input()), ToRegister(result()));
  __ JumpIfNotRoot(ToRegister(object_input()), RootIndex::kTheHoleValue, &done);
  __ LoadRoot(ToRegister(result()), RootIndex::kUndefinedValue);
  __ bind(&done);
}

int ConvertReceiver::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kToObject>::type;
  return D::GetStackParameterCount();
}
void ConvertReceiver::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kToObject>::type;
  UseFixed(receiver_input(), D::GetRegisterParameter(D::kInput));
  DefineAsFixed(this, kReturnRegister0);
}
void ConvertReceiver::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  Label convert_to_object, done;
  Register receiver = ToRegister(receiver_input());
  __ JumpIfSmi(receiver, &convert_to_object, Label::Distance::kNear);
  __ JumpIfJSAnyIsNotPrimitive(receiver, &done);

  compiler::JSHeapBroker* broker = masm->compilation_info()->broker();
  if (mode_ != ConvertReceiverMode::kNotNullOrUndefined) {
    Label convert_global_proxy;
    __ JumpIfRoot(receiver, RootIndex::kUndefinedValue, &convert_global_proxy,
                  Label::Distance::kNear);
    __ JumpIfNotRoot(receiver, RootIndex::kNullValue, &convert_to_object,
                     Label::Distance::kNear);
    __ bind(&convert_global_proxy);
    // Patch receiver to global proxy.
    __ Move(ToRegister(result()),
            native_context_.global_proxy_object(broker).object());
    __ Jump(&done);
  }

  __ bind(&convert_to_object);
  __ CallBuiltin<Builtin::kToObject>(native_context_.object(),
                                     receiver_input());
  __ bind(&done);
}

int CheckConstructResult::MaxCallStackArgs() const { return 0; }
void CheckConstructResult::SetValueLocationConstraints() {
  UseRegister(construct_result_input());
  UseRegister(implicit_receiver_input());
  DefineSameAsFirst(this);
}
void CheckConstructResult::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  Register construct_result = ToRegister(construct_result_input());
  Register implicit_receiver = ToRegister(implicit_receiver_input());
  Register result_reg = ToRegister(result());
  DCHECK_EQ(construct_result, result_reg);

  // If the result is an object (in the ECMA sense), we should get rid
  // of the receiver and use the result; see ECMA-262 section 13.2.2-7
  // on page 74.
  Label done, use_receiver;

  // If the result is undefined, we'll use the implicit receiver.
  __ JumpIfRoot(construct_result, RootIndex::kUndefinedValue, &use_receiver,
                Label::Distance::kNear);

  // If the result is a smi, it is *not* an object in the ECMA sense.
  __ JumpIfSmi(construct_result, &use_receiver, Label::Distance::kNear);

  // Check if the type of the result is not an object in the ECMA sense.
  __ JumpIfJSAnyIsNotPrimitive(construct_result, &done, Label::Distance::kNear);

  // Throw away the result of the constructor invocation and use the
  // implicit receiver as the result.
  __ bind(&use_receiver);
  __ JumpIfRoot(
      implicit_receiver, RootIndex::kTheHoleValue,
      __ MakeDeferredCode(
          [](MaglevAssembler* masm, CheckConstructResult* node) {
            __ Move(kContextRegister, masm->native_context().object());
            __ CallRuntime(Runtime::kThrowConstructorReturnedNonObject);
            masm->DefineExceptionHandlerAndLazyDeoptPoint(node);
            __ Abort(AbortReason::kUnexpectedReturnFromThrow);
          },
          this));
  __ Move(result_reg, implicit_receiver);

  __ bind(&done);
}

int CreateObjectLiteral::MaxCallStackArgs() const {
  DCHECK_EQ(Runtime::FunctionForId(Runtime::kCreateObjectLiteral)->nargs, 4);
  return 4;
}
void CreateObjectLiteral::SetValueLocationConstraints() {
  DefineAsFixed(this, kReturnRegister0);
}
void CreateObjectLiteral::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  __ Move(kContextRegister, masm->native_context().object());
  __ Push(feedback().vector, TaggedIndex::FromIntptr(feedback().index()),
          boilerplate_descriptor().object(), Smi::FromInt(flags()));
  __ CallRuntime(Runtime::kCreateObjectLiteral, 4);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int CreateShallowArrayLiteral::MaxCallStackArgs() const {
  using D =
      CallInterfaceDescriptorFor<Builtin::kCreateShallowArrayLiteral>::type;
  return D::GetStackParameterCount();
}
void CreateShallowArrayLiteral::SetValueLocationConstraints() {
  DefineAsFixed(this, kReturnRegister0);
}
void CreateShallowArrayLiteral::GenerateCode(MaglevAssembler* masm,
                                             const ProcessingState& state) {
  __ CallBuiltin<Builtin::kCreateShallowArrayLiteral>(
      masm->native_context().object(),              // context
      feedback().vector,                            // feedback vector
      TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
      constant_elements().object(),                 // constant elements
      Smi::FromInt(flags())                         // flags
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int CreateArrayLiteral::MaxCallStackArgs() const {
  DCHECK_EQ(Runtime::FunctionForId(Runtime::kCreateArrayLiteral)->nargs, 4);
  return 4;
}
void CreateArrayLiteral::SetValueLocationConstraints() {
  DefineAsFixed(this, kReturnRegister0);
}
void CreateArrayLiteral::GenerateCode(MaglevAssembler* masm,
                                      const ProcessingState& state) {
  __ Move(kContextRegister, masm->native_context().object());
  __ Push(feedback().vector, TaggedIndex::FromIntptr(feedback().index()),
          constant_elements().object(), Smi::FromInt(flags()));
  __ CallRuntime(Runtime::kCreateArrayLiteral, 4);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int CreateShallowObjectLiteral::MaxCallStackArgs() const {
  using D =
      CallInterfaceDescriptorFor<Builtin::kCreateShallowObjectLiteral>::type;
  return D::GetStackParameterCount();
}
void CreateShallowObjectLiteral::SetValueLocationConstraints() {
  DefineAsFixed(this, kReturnRegister0);
}
void CreateShallowObjectLiteral::GenerateCode(MaglevAssembler* masm,
                                              const ProcessingState& state) {
  __ CallBuiltin<Builtin::kCreateShallowObjectLiteral>(
      masm->native_context().object(),              // context
      feedback().vector,                            // feedback vector
      TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
      boilerplate_descriptor().object(),            // desc
      Smi::FromInt(flags())                         // flags
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void AllocateRaw::SetValueLocationConstraints() { DefineAsRegister(this); }

void AllocateRaw::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  __ Allocate(register_snapshot(), ToRegister(result()), size(),
              allocation_type());
}

int CreateClosure::MaxCallStackArgs() const {
  DCHECK_EQ(Runtime::FunctionForId(pretenured() ? Runtime::kNewClosure_Tenured
                                                : Runtime::kNewClosure)
                ->nargs,
            2);
  return 2;
}
void CreateClosure::SetValueLocationConstraints() {
  UseFixed(context(), kContextRegister);
  DefineAsFixed(this, kReturnRegister0);
}
void CreateClosure::GenerateCode(MaglevAssembler* masm,
                                 const ProcessingState& state) {
  Runtime::FunctionId function_id =
      pretenured() ? Runtime::kNewClosure_Tenured : Runtime::kNewClosure;
  __ Push(shared_function_info().object(), feedback_cell().object());
  __ CallRuntime(function_id);
}

int FastCreateClosure::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kFastNewClosure>::type;
  return D::GetStackParameterCount();
}
void FastCreateClosure::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kFastNewClosure>::type;
  static_assert(D::HasContextParameter());
  UseFixed(context(), D::ContextRegister());
  DefineAsFixed(this, kReturnRegister0);
}
void FastCreateClosure::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  __ CallBuiltin<Builtin::kFastNewClosure>(
      context(),                        // context
      shared_function_info().object(),  // shared function info
      feedback_cell().object()          // feedback cell
  );
  masm->DefineLazyDeoptPoint(lazy_deopt_info());
}

int CreateFunctionContext::MaxCallStackArgs() const {
  if (scope_type() == FUNCTION_SCOPE) {
    using D = CallInterfaceDescriptorFor<
        Builtin::kFastNewFunctionContextFunction>::type;
    return D::GetStackParameterCount();
  } else {
    using D =
        CallInterfaceDescriptorFor<Builtin::kFastNewFunctionContextEval>::type;
    return D::GetStackParameterCount();
  }
}
void CreateFunctionContext::SetValueLocationConstraints() {
  DCHECK_LE(slot_count(),
            static_cast<uint32_t>(
                ConstructorBuiltins::MaximumFunctionContextSlots()));
  if (scope_type() == FUNCTION_SCOPE) {
    using D = CallInterfaceDescriptorFor<
        Builtin::kFastNewFunctionContextFunction>::type;
    static_assert(D::HasContextParameter());
    UseFixed(context(), D::ContextRegister());
  } else {
    DCHECK_EQ(scope_type(), ScopeType::EVAL_SCOPE);
    using D =
        CallInterfaceDescriptorFor<Builtin::kFastNewFunctionContextEval>::type;
    static_assert(D::HasContextParameter());
    UseFixed(context(), D::ContextRegister());
  }
  DefineAsFixed(this, kReturnRegister0);
}
void CreateFunctionContext::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  if (scope_type() == FUNCTION_SCOPE) {
    __ CallBuiltin<Builtin::kFastNewFunctionContextFunction>(
        context(),              // context
        scope_info().object(),  // scope info
        slot_count()            // slots
    );
  } else {
    __ CallBuiltin<Builtin::kFastNewFunctionContextEval>(
        context(),              // context
        scope_info().object(),  // scope info
        slot_count()            // slots
    );
  }
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int CreateRegExpLiteral::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kCreateRegExpLiteral>::type;
  return D::GetStackParameterCount();
}
void CreateRegExpLiteral::SetValueLocationConstraints() {
  DefineAsFixed(this, kReturnRegister0);
}
void CreateRegExpLiteral::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  __ CallBuiltin<Builtin::kCreateRegExpLiteral>(
      masm->native_context().object(),              // context
      feedback().vector,                            // feedback vector
      TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
      pattern().object(),                           // pattern
      Smi::FromInt(flags())                         // flags
  );
}

int GetTemplateObject::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kGetTemplateObject>::type;
  return D::GetStackParameterCount();
}
void GetTemplateObject::SetValueLocationConstraints() {
  using D = GetTemplateObjectDescriptor;
  UseFixed(description(), D::GetRegisterParameter(D::kDescription));
  DefineAsFixed(this, kReturnRegister0);
}
void GetTemplateObject::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  __ CallBuiltin<Builtin::kGetTemplateObject>(
      masm->native_context().object(),  // context
      shared_function_info_.object(),   // shared function info
      description(),                    // description
      feedback().index(),               // feedback slot
      feedback().vector                 // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int HasInPrototypeChain::MaxCallStackArgs() const {
  DCHECK_EQ(2, Runtime::FunctionForId(Runtime::kHasInPrototypeChain)->nargs);
  return 2;
}
void HasInPrototypeChain::SetValueLocationConstraints() {
  UseRegister(object());
  DefineAsRegister(this);
  set_temporaries_needed(2);
}
void HasInPrototypeChain::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register object_reg = ToRegister(object());
  Register result_reg = ToRegister(result());

  Label return_false, return_true;
  ZoneLabelRef done(masm);

  __ JumpIfSmi(object_reg, &return_false, Label::kNear);

  // Loop through the prototype chain looking for the {prototype}.
  Register map = temps.Acquire();
  __ LoadMap(map, object_reg);
  Label loop;
  {
    __ bind(&loop);
    Register scratch = temps.Acquire();
    // Check if we can determine the prototype directly from the {object_map}.
    ZoneLabelRef if_objectisdirect(masm);
    Register instance_type = scratch;
    __ CompareInstanceTypeRange(map, instance_type, FIRST_TYPE,
                                LAST_SPECIAL_RECEIVER_TYPE);
    __ JumpToDeferredIf(
        kLessThan,
        [](MaglevAssembler* masm, RegisterSnapshot snapshot,
           Register object_reg, Register map, Register instance_type,
           Register result_reg, HasInPrototypeChain* node,
           ZoneLabelRef if_objectisdirect, ZoneLabelRef done) {
          Label return_runtime;
          // The {object_map} is a special receiver map or a primitive map,
          // check if we need to use the if_objectisspecial path in the runtime.
          __ JumpIfEqual(instance_type, JS_PROXY_TYPE, &return_runtime);

          Register object_bitfield = instance_type;
          __ LoadByte(object_bitfield,
                      FieldMemOperand(map, Map::kBitFieldOffset));
          int mask = Map::Bits1::HasNamedInterceptorBit::kMask |
                     Map::Bits1::IsAccessCheckNeededBit::kMask;
          __ TestInt32AndJumpIfAllClear(object_bitfield, mask,
                                        *if_objectisdirect);

          __ bind(&return_runtime);
          {
            snapshot.live_registers.clear(result_reg);
            SaveRegisterStateForCall save_register_state(masm, snapshot);
            __ Move(kContextRegister, masm->native_context().object());
            __ Push(object_reg, node->prototype().object());
            __ CallRuntime(Runtime::kHasInPrototypeChain, 2);
            masm->DefineExceptionHandlerPoint(node);
            save_register_state.DefineSafepointWithLazyDeopt(
                node->lazy_deopt_info());
            __ Move(result_reg, kReturnRegister0);
          }
          __ Jump(*done);
        },
        register_snapshot(), object_reg, map, instance_type, result_reg, this,
        if_objectisdirect, done);
    instance_type = Register::no_reg();

    __ bind(*if_objectisdirect);
    // Check the current {object} prototype.
    Register object_prototype = scratch;
    __ LoadTaggedField(object_prototype, map, Map::kPrototypeOffset);
    __ JumpIfRoot(object_prototype, RootIndex::kNullValue, &return_false,
                  Label::kNear);
    __ CompareTagged(object_prototype, prototype().object());
    __ JumpIf(kEqual, &return_true, Label::kNear);

    // Continue with the prototype.
    __ AssertNotSmi(object_prototype);
    __ LoadMap(map, object_prototype);
    __ Jump(&loop);
  }

  __ bind(&return_true);
  __ LoadRoot(result_reg, RootIndex::kTrueValue);
  __ Jump(*done, Label::kNear);

  __ bind(&return_false);
  __ LoadRoot(result_reg, RootIndex::kFalseValue);
  __ bind(*done);
}

void DebugBreak::SetValueLocationConstraints() {}
void DebugBreak::GenerateCode(MaglevAssembler* masm,
                              const ProcessingState& state) {
  __ DebugBreak();
}

int Abort::MaxCallStackArgs() const {
  DCHECK_EQ(Runtime::FunctionForId(Runtime::kAbort)->nargs, 1);
  return 1;
}
void Abort::SetValueLocationConstraints() {}
void Abort::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {
  __ Push(Smi::FromInt(static_cast<int>(reason())));
  __ CallRuntime(Runtime::kAbort, 1);
  __ Trap();
}

void LogicalNot::SetValueLocationConstraints() {
  UseAny(value());
  DefineAsRegister(this);
}
void LogicalNot::GenerateCode(MaglevAssembler* masm,
                              const ProcessingState& state) {
  if (v8_flags.debug_code) {
    // LogicalNot expects either TrueValue or FalseValue.
    Label next;
    __ JumpIf(__ IsRootConstant(value(), RootIndex::kFalseValue), &next);
    __ JumpIf(__ IsRootConstant(value(), RootIndex::kTrueValue), &next);
    __ Abort(AbortReason::kUnexpectedValue);
    __ bind(&next);
  }

  Label return_false, done;
  __ JumpIf(__ IsRootConstant(value(), RootIndex::kTrueValue), &return_false);
  __ LoadRoot(ToRegister(result()), RootIndex::kTrueValue);
  __ Jump(&done);

  __ bind(&return_false);
  __ LoadRoot(ToRegister(result()), RootIndex::kFalseValue);

  __ bind(&done);
}

int LoadNamedGeneric::MaxCallStackArgs() const {
  return LoadWithVectorDescriptor::GetStackParameterCount();
}
void LoadNamedGeneric::SetValueLocationConstraints() {
  using D = LoadWithVectorDescriptor;
  UseFixed(context(), kContextRegister);
  UseFixed(object_input(), D::GetRegisterParameter(D::kReceiver));
  DefineAsFixed(this, kReturnRegister0);
}
void LoadNamedGeneric::GenerateCode(MaglevAssembler* masm,
                                    const ProcessingState& state) {
  __ CallBuiltin<Builtin::kLoadIC>(
      context(),                                    // context
      object_input(),                               // receiver
      name().object(),                              // name
      TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
      feedback().vector                             // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int LoadNamedFromSuperGeneric::MaxCallStackArgs() const {
  return LoadWithReceiverAndVectorDescriptor::GetStackParameterCount();
}
void LoadNamedFromSuperGeneric::SetValueLocationConstraints() {
  using D = LoadWithReceiverAndVectorDescriptor;
  UseFixed(context(), kContextRegister);
  UseFixed(receiver(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(lookup_start_object(),
           D::GetRegisterParameter(D::kLookupStartObject));
  DefineAsFixed(this, kReturnRegister0);
}
void LoadNamedFromSuperGeneric::GenerateCode(MaglevAssembler* masm,
                                             const ProcessingState& state) {
  __ CallBuiltin<Builtin::kLoadSuperIC>(
      context(),                                    // context
      receiver(),                                   // receiver
      lookup_start_object(),                        // lookup start object
      name().object(),                              // name
      TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
      feedback().vector                             // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int SetNamedGeneric::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kStoreIC>::type;
  return D::GetStackParameterCount();
}
void SetNamedGeneric::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kStoreIC>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object_input(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(value_input(), D::GetRegisterParameter(D::kValue));
  DefineAsFixed(this, kReturnRegister0);
}
void SetNamedGeneric::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  __ CallBuiltin<Builtin::kStoreIC>(
      context(),                                    // context
      object_input(),                               // receiver
      name().object(),                              // name
      value_input(),                                // value
      TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
      feedback().vector                             // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int DefineNamedOwnGeneric::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kDefineNamedOwnIC>::type;
  return D::GetStackParameterCount();
}
void DefineNamedOwnGeneric::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kDefineNamedOwnIC>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object_input(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(value_input(), D::GetRegisterParameter(D::kValue));
  DefineAsFixed(this, kReturnRegister0);
}
void DefineNamedOwnGeneric::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  __ CallBuiltin<Builtin::kDefineNamedOwnIC>(
      context(),                                    // context
      object_input(),                               // receiver
      name().object(),                              // name
      value_input(),                                // value
      TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
      feedback().vector                             // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void UpdateJSArrayLength::SetValueLocationConstraints() {
  UseRegister(object_input());
  UseAndClobberRegister(index_input());
  UseRegister(length_input());
}
void UpdateJSArrayLength::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register index = ToRegister(index_input());
  Register length = ToRegister(length_input());
  Label done;
  if (v8_flags.debug_code) {
    __ IsObjectType(object, JS_ARRAY_TYPE);
    __ Assert(kEqual, AbortReason::kUnexpectedValue);
    static_assert(Internals::IsValidSmi(FixedArray::kMaxLength),
                  "MaxLength not a Smi");
    __ CompareInt32(index, FixedArray::kMaxLength);
    __ Assert(kUnsignedLessThan, AbortReason::kUnexpectedValue);
  }
  __ CompareInt32(index, length);
  __ JumpIf(kUnsignedLessThan, &done);
  __ IncrementInt32(index);  // This cannot overflow.
  __ SmiTag(index);
  __ StoreTaggedSignedField(object, JSArray::kLengthOffset, index);
  __ bind(&done);
}

void EnsureWritableFastElements::SetValueLocationConstraints() {
  UseRegister(elements_input());
  UseRegister(object_input());
  set_temporaries_needed(1);
  DefineSameAsFirst(this);
}
void EnsureWritableFastElements::GenerateCode(MaglevAssembler* masm,
                                              const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register elements = ToRegister(elements_input());
  DCHECK_EQ(elements, ToRegister(result()));
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();
  __ EnsureWritableFastElements(register_snapshot(), elements, object, scratch);
}

void MaybeGrowAndEnsureWritableFastElements::SetValueLocationConstraints() {
  UseRegister(elements_input());
  UseRegister(object_input());
  UseRegister(index_input());
  UseRegister(elements_length_input());
  if (IsSmiOrObjectElementsKind(elements_kind())) {
    set_temporaries_needed(1);
  }
  DefineSameAsFirst(this);
}
void MaybeGrowAndEnsureWritableFastElements::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register elements = ToRegister(elements_input());
  Register object = ToRegister(object_input());
  Register index = ToRegister(index_input());
  Register elements_length = ToRegister(elements_length_input());
  DCHECK_EQ(elements, ToRegister(result()));

  ZoneLabelRef done(masm);
  __ CompareInt32(index, elements_length);
  __ JumpToDeferredIf(
      kUnsignedGreaterThanEqual,
      [](MaglevAssembler* masm, ZoneLabelRef done, Register object,
         Register index, Register result_reg,
         MaybeGrowAndEnsureWritableFastElements* node) {
        {
          RegisterSnapshot snapshot = node->register_snapshot();
          AddDeoptRegistersToSnapshot(&snapshot, node->eager_deopt_info());
          snapshot.live_registers.clear(result_reg);
          snapshot.live_tagged_registers.clear(result_reg);
          SaveRegisterStateForCall save_register_state(masm, snapshot);
          using D = GrowArrayElementsDescriptor;
          if (index == D::GetRegisterParameter(D::kObject)) {
            // That implies that the first parameter move will clobber the index
            // value. So we use the result register as temporary.
            // TODO(leszeks): Use parallel moves to resolve cases like this.
            __ SmiTag(result_reg, index);
            index = result_reg;
          } else {
            __ SmiTag(index);
          }
          if (IsDoubleElementsKind(node->elements_kind())) {
            __ CallBuiltin<Builtin::kGrowFastDoubleElements>(object, index);
          } else {
            __ CallBuiltin<Builtin::kGrowFastSmiOrObjectElements>(object,
                                                                  index);
          }
          save_register_state.DefineSafepoint();
          __ Move(result_reg, kReturnRegister0);
        }
        Condition is_smi = __ CheckSmi(result_reg);
        __ EmitEagerDeoptIf(is_smi, DeoptimizeReason::kCouldNotGrowElements,
                            node);
        __ Jump(*done);
      },
      done, object, index, elements, this);

  if (IsSmiOrObjectElementsKind(elements_kind())) {
    MaglevAssembler::ScratchRegisterScope temps(masm);
    Register scratch = temps.Acquire();
    __ EnsureWritableFastElements(register_snapshot(), elements, object,
                                  scratch);
  }

  __ bind(*done);
}

int SetKeyedGeneric::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kKeyedStoreIC>::type;
  return D::GetStackParameterCount();
}
void SetKeyedGeneric::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kKeyedStoreIC>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object_input(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(key_input(), D::GetRegisterParameter(D::kName));
  UseFixed(value_input(), D::GetRegisterParameter(D::kValue));
  DefineAsFixed(this, kReturnRegister0);
}
void SetKeyedGeneric::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  __ CallBuiltin<Builtin::kKeyedStoreIC>(
      context(),                                    // context
      object_input(),                               // receiver
      key_input(),                                  // name
      value_input(),                                // value
      TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
      feedback().vector                             // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int DefineKeyedOwnGeneric::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kDefineKeyedOwnIC>::type;
  return D::GetStackParameterCount();
}
void DefineKeyedOwnGeneric::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kDefineKeyedOwnIC>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object_input(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(key_input(), D::GetRegisterParameter(D::kName));
  UseFixed(value_input(), D::GetRegisterParameter(D::kValue));
  UseFixed(flags_input(), D::GetRegisterParameter(D::kFlags));
  DefineAsFixed(this, kReturnRegister0);
}
void DefineKeyedOwnGeneric::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  __ CallBuiltin<Builtin::kDefineKeyedOwnIC>(
      context(),                                    // context
      object_input(),                               // receiver
      key_input(),                                  // name
      value_input(),                                // value
      flags_input(),                                // flags
      TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
      feedback().vector                             // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int StoreInArrayLiteralGeneric::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kStoreInArrayLiteralIC>::type;
  return D::GetStackParameterCount();
}
void StoreInArrayLiteralGeneric::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kStoreInArrayLiteralIC>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object_input(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(name_input(), D::GetRegisterParameter(D::kName));
  UseFixed(value_input(), D::GetRegisterParameter(D::kValue));
  DefineAsFixed(this, kReturnRegister0);
}
void StoreInArrayLiteralGeneric::GenerateCode(MaglevAssembler* masm,
                                              const ProcessingState& state) {
  __ CallBuiltin<Builtin::kStoreInArrayLiteralIC>(
      context(),                                    // context
      object_input(),                               // receiver
      name_input(),                                 // name
      value_input(),                                // value
      TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
      feedback().vector                             // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void GeneratorRestoreRegister::SetValueLocationConstraints() {
  UseRegister(array_input());
  UseRegister(stale_input());
  DefineAsRegister(this);
  set_temporaries_needed(1);
}
void GeneratorRestoreRegister::GenerateCode(MaglevAssembler* masm,
                                            const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register temp = temps.Acquire();
  Register array = ToRegister(array_input());
  Register stale = ToRegister(stale_input());
  Register result_reg = ToRegister(result());

  // The input and the output can alias, if that happens we use a temporary
  // register and a move at the end.
  Register value = (array == result_reg ? temp : result_reg);

  // Loads the current value in the generator register file.
  __ DecompressTagged(
      value, FieldMemOperand(array, FixedArray::OffsetOfElementAt(index())));

  // And trashs it with StaleRegisterConstant.
  DCHECK(stale_input().node()->Is<RootConstant>());
  __ StoreTaggedFieldNoWriteBarrier(
      array, FixedArray::OffsetOfElementAt(index()), stale);

  if (value != result_reg) {
    __ Move(result_reg, value);
  }
}

int GeneratorStore::MaxCallStackArgs() const {
  return WriteBarrierDescriptor::GetStackParameterCount();
}
void GeneratorStore::SetValueLocationConstraints() {
  UseAny(context_input());
  UseRegister(generator_input());
  for (int i = 0; i < num_parameters_and_registers(); i++) {
    UseAny(parameters_and_registers(i));
  }
  RequireSpecificTemporary(WriteBarrierDescriptor::ObjectRegister());
  RequireSpecificTemporary(WriteBarrierDescriptor::SlotAddressRegister());
}
void GeneratorStore::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  Register generator = ToRegister(generator_input());
  Register array = WriteBarrierDescriptor::ObjectRegister();
  __ LoadTaggedField(array, generator,
                     JSGeneratorObject::kParametersAndRegistersOffset);

  RegisterSnapshot register_snapshot_during_store = register_snapshot();
  // Include the array and generator registers in the register snapshot while
  // storing parameters and registers, to avoid the write barrier clobbering
  // them.
  register_snapshot_during_store.live_registers.set(array);
  register_snapshot_during_store.live_tagged_registers.set(array);
  register_snapshot_during_store.live_registers.set(generator);
  register_snapshot_during_store.live_tagged_registers.set(generator);
  for (int i = 0; i < num_parameters_and_registers(); i++) {
    // Use WriteBarrierDescriptor::SlotAddressRegister() as the temporary for
    // the value -- it'll be clobbered by StoreTaggedFieldWithWriteBarrier since
    // it's not in the register snapshot, but that's ok, and a clobberable value
    // register lets the write barrier emit slightly better code.
    Input value_input = parameters_and_registers(i);
    Register value = __ FromAnyToRegister(
        value_input, WriteBarrierDescriptor::SlotAddressRegister());
    // Include the value register in the live set, in case it is used by future
    // inputs.
    register_snapshot_during_store.live_registers.set(value);
    register_snapshot_during_store.live_tagged_registers.set(value);
    __ StoreTaggedFieldWithWriteBarrier(
        array, FixedArray::OffsetOfElementAt(i), value,
        register_snapshot_during_store,
        value_input.node()->decompresses_tagged_result()
            ? MaglevAssembler::kValueIsDecompressed
            : MaglevAssembler::kValueIsCompressed,
        MaglevAssembler::kValueCanBeSmi);
  }

  __ StoreTaggedSignedField(generator, JSGeneratorObject::kContinuationOffset,
                            Smi::FromInt(suspend_id()));
  __ StoreTaggedSignedField(generator,
                            JSGeneratorObject::kInputOrDebugPosOffset,
                            Smi::FromInt(bytecode_offset()));

  // Use WriteBarrierDescriptor::SlotAddressRegister() as the scratch
  // register, see comment above. At this point we no longer need to preserve
  // the array or generator registers, so use the original register snapshot.
  Register context = __ FromAnyToRegister(
      context_input(), WriteBarrierDescriptor::SlotAddressRegister());
  __ StoreTaggedFieldWithWriteBarrier(
      generator, JSGeneratorObject::kContextOffset, context,
      register_snapshot(),
      context_input().node()->decompresses_tagged_result()
          ? MaglevAssembler::kValueIsDecompressed
          : MaglevAssembler::kValueIsCompressed,
      MaglevAssembler::kValueCannotBeSmi);
}

int GetKeyedGeneric::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kKeyedLoadIC>::type;
  return D::GetStackParameterCount();
}
void GetKeyedGeneric::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kKeyedLoadIC>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object_input(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(key_input(), D::GetRegisterParameter(D::kName));
  DefineAsFixed(this, kReturnRegister0);
}
void GetKeyedGeneric::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  __ CallBuiltin<Builtin::kKeyedLoadIC>(
      context(),                                    // context
      object_input(),                               // receiver
      key_input(),                                  // name
      TaggedIndex::FromIntptr(feedback().index()),  // feedback slot
      feedback().vector                             // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void Float64ToTagged::SetValueLocationConstraints() {
  UseRegister(input());
  DefineAsRegister(this);
}
void Float64ToTagged::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  DoubleRegister value = ToDoubleRegister(input());
  Register object = ToRegister(result());
  Label box, done;
  if (canonicalize_smi()) {
    __ TryTruncateDoubleToInt32(object, value, &box);
    __ SmiTagInt32AndJumpIfFail(object, &box);
    __ Jump(&done, Label::kNear);
    __ bind(&box);
  }
  __ AllocateHeapNumber(register_snapshot(), object, value);
  if (canonicalize_smi()) {
    __ bind(&done);
  }
}

void HoleyFloat64ToTagged::SetValueLocationConstraints() {
  UseRegister(input());
  DefineAsRegister(this);
}
void HoleyFloat64ToTagged::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  ZoneLabelRef done(masm);
  DoubleRegister value = ToDoubleRegister(input());
  Register object = ToRegister(result());
  Label box;
  if (canonicalize_smi()) {
    __ TryTruncateDoubleToInt32(object, value, &box);
    __ SmiTagInt32AndJumpIfFail(object, &box);
    __ Jump(*done, Label::kNear);
    __ bind(&box);
  }
  // Using return as scratch register.
  __ JumpIfHoleNan(
      value, ToRegister(result()),
      __ MakeDeferredCode(
          [](MaglevAssembler* masm, Register object, ZoneLabelRef done) {
            // TODO(leszeks): Evaluate whether this is worth deferring.
            __ LoadRoot(object, RootIndex::kUndefinedValue);
            __ Jump(*done);
          },
          object, done));
  __ AllocateHeapNumber(register_snapshot(), object, value);
  __ bind(*done);
}

void Float64Round::SetValueLocationConstraints() {
  UseRegister(input());
  DefineAsRegister(this);
  if (kind_ == Kind::kNearest) {
    set_double_temporaries_needed(1);
  }
}

void CheckedSmiTagFloat64::SetValueLocationConstraints() {
  UseRegister(input());
  DefineAsRegister(this);
}
void CheckedSmiTagFloat64::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  DoubleRegister value = ToDoubleRegister(input());
  Register object = ToRegister(result());

  __ TryTruncateDoubleToInt32(
      object, value, __ GetDeoptLabel(this, DeoptimizeReason::kNotASmi));
  __ SmiTagInt32AndJumpIfFail(
      object, __ GetDeoptLabel(this, DeoptimizeReason::kNotASmi));
}

void StoreFloat64::SetValueLocationConstraints() {
  UseRegister(object_input());
  UseRegister(value_input());
}
void StoreFloat64::GenerateCode(MaglevAssembler* masm,
                                const ProcessingState& state) {
  Register object = ToRegister(object_input());
  DoubleRegister value = ToDoubleRegister(value_input());

  __ AssertNotSmi(object);
  __ StoreFloat64(FieldMemOperand(object, offset()), value);
}

void StoreTaggedFieldNoWriteBarrier::SetValueLocationConstraints() {
  UseRegister(object_input());
  UseRegister(value_input());
}
void StoreTaggedFieldNoWriteBarrier::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register value = ToRegister(value_input());

  __ AssertNotSmi(object);
  __ StoreTaggedFieldNoWriteBarrier(object, offset(), value);
}

int StringAt::MaxCallStackArgs() const {
  DCHECK_EQ(Runtime::FunctionForId(Runtime::kStringCharCodeAt)->nargs, 2);
  return std::max(2, AllocateDescriptor::GetStackParameterCount());
}
void StringAt::SetValueLocationConstraints() {
  UseAndClobberRegister(string_input());
  UseAndClobberRegister(index_input());
  DefineAsRegister(this);
  set_temporaries_needed(1);
}
void StringAt::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();
  Register result_string = ToRegister(result());
  Register string = ToRegister(string_input());
  Register index = ToRegister(index_input());
  Register char_code = string;

  ZoneLabelRef done(masm);
  Label cached_one_byte_string;

  RegisterSnapshot save_registers = register_snapshot();
  __ StringCharCodeOrCodePointAt(
      BuiltinStringPrototypeCharCodeOrCodePointAt::kCharCodeAt, save_registers,
      char_code, string, index, scratch, &cached_one_byte_string);
  __ StringFromCharCode(save_registers, &cached_one_byte_string, result_string,
                        char_code, scratch);
}

void StringLength::SetValueLocationConstraints() {
  UseRegister(object_input());
  DefineAsRegister(this);
}
void StringLength::GenerateCode(MaglevAssembler* masm,
                                const ProcessingState& state) {
  __ StringLength(ToRegister(result()), ToRegister(object_input()));
}

void StringConcat::SetValueLocationConstraints() {
  using D = StringAdd_CheckNoneDescriptor;
  UseFixed(lhs(), D::GetRegisterParameter(D::kLeft));
  UseFixed(rhs(), D::GetRegisterParameter(D::kRight));
  DefineAsFixed(this, kReturnRegister0);
}
void StringConcat::GenerateCode(MaglevAssembler* masm,
                                const ProcessingState& state) {
  __ CallBuiltin<Builtin::kStringAdd_CheckNone>(
      masm->native_context().object(),  // context
      lhs(),                            // left
      rhs()                             // right
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
  DCHECK_EQ(kReturnRegister0, ToRegister(result()));
}

void StringEqual::SetValueLocationConstraints() {
  using D = StringEqualDescriptor;
  UseFixed(lhs(), D::GetRegisterParameter(D::kLeft));
  UseFixed(rhs(), D::GetRegisterParameter(D::kRight));
  set_temporaries_needed(1);
  RequireSpecificTemporary(D::GetRegisterParameter(D::kLength));
  DefineAsFixed(this, kReturnRegister0);
}
void StringEqual::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  using D = StringEqualDescriptor;
  Label done, if_equal, if_not_equal;
  Register left = ToRegister(lhs());
  Register right = ToRegister(rhs());
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register left_length = temps.Acquire();
  Register right_length = D::GetRegisterParameter(D::kLength);

  __ CmpTagged(left, right);
  __ JumpIf(kEqual, &if_equal, Label::Distance::kNear);

  __ StringLength(left_length, left);
  __ StringLength(right_length, right);
  __ CompareInt32(left_length, right_length);
  __ JumpIf(kNotEqual, &if_not_equal, Label::Distance::kNear);

  // The inputs are already in the right registers. The |left| and |right|
  // inputs were required to come in in the left/right inputs of the builtin,
  // and the |length| input of the builtin is where we loaded the length of the
  // right string (which matches the length of the left string when we get
  // here).
  DCHECK_EQ(right_length, D::GetRegisterParameter(D::kLength));
  __ CallBuiltin<Builtin::kStringEqual>(lhs(), rhs(),
                                        D::GetRegisterParameter(D::kLength));
  masm->DefineLazyDeoptPoint(this->lazy_deopt_info());
  __ Jump(&done, Label::Distance::kNear);

  __ bind(&if_equal);
  __ LoadRoot(ToRegister(result()), RootIndex::kTrueValue);
  __ Jump(&done, Label::Distance::kNear);

  __ bind(&if_not_equal);
  __ LoadRoot(ToRegister(result()), RootIndex::kFalseValue);

  __ bind(&done);
}

void TaggedEqual::SetValueLocationConstraints() {
  UseRegister(lhs());
  UseRegister(rhs());
  DefineAsRegister(this);
}
void TaggedEqual::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  Label done, if_equal;
  __ CmpTagged(ToRegister(lhs()), ToRegister(rhs()));
  __ JumpIf(kEqual, &if_equal, Label::Distance::kNear);
  __ LoadRoot(ToRegister(result()), RootIndex::kFalseValue);
  __ Jump(&done);
  __ bind(&if_equal);
  __ LoadRoot(ToRegister(result()), RootIndex::kTrueValue);
  __ bind(&done);
}

void TaggedNotEqual::SetValueLocationConstraints() {
  UseRegister(lhs());
  UseRegister(rhs());
  DefineAsRegister(this);
}
void TaggedNotEqual::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  Label done, if_equal;
  __ CmpTagged(ToRegister(lhs()), ToRegister(rhs()));
  __ JumpIf(kEqual, &if_equal, Label::Distance::kNear);
  __ LoadRoot(ToRegister(result()), RootIndex::kTrueValue);
  __ Jump(&done);
  __ bind(&if_equal);
  __ LoadRoot(ToRegister(result()), RootIndex::kFalseValue);
  __ bind(&done);
}

int TestInstanceOf::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kInstanceOf_WithFeedback>::type;
  return D::GetStackParameterCount();
}
void TestInstanceOf::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kInstanceOf_WithFeedback>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object(), D::GetRegisterParameter(D::kLeft));
  UseFixed(callable(), D::GetRegisterParameter(D::kRight));
  DefineAsFixed(this, kReturnRegister0);
}
void TestInstanceOf::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  __ CallBuiltin<Builtin::kInstanceOf_WithFeedback>(
      context(),           // context
      object(),            // left
      callable(),          // right
      feedback().index(),  // feedback slot
      feedback().vector    // feedback vector
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void TestTypeOf::SetValueLocationConstraints() {
  UseRegister(value());
  DefineAsRegister(this);
}
void TestTypeOf::GenerateCode(MaglevAssembler* masm,
                              const ProcessingState& state) {
  Register object = ToRegister(value());
  Label is_true, is_false, done;
  __ TestTypeOf(object, literal_, &is_true, Label::Distance::kNear, true,
                &is_false, Label::Distance::kNear, false);
  // Fallthrough into true.
  __ bind(&is_true);
  __ LoadRoot(ToRegister(result()), RootIndex::kTrueValue);
  __ Jump(&done, Label::Distance::kNear);
  __ bind(&is_false);
  __ LoadRoot(ToRegister(result()), RootIndex::kFalseValue);
  __ bind(&done);
}

void ToBoolean::SetValueLocationConstraints() {
  UseRegister(value());
  DefineAsRegister(this);
}
void ToBoolean::GenerateCode(MaglevAssembler* masm,
                             const ProcessingState& state) {
  Register object = ToRegister(value());
  Register return_value = ToRegister(result());
  Label done;
  ZoneLabelRef object_is_true(masm), object_is_false(masm);
  // TODO(leszeks): We're likely to be calling this on an existing boolean --
  // maybe that's a case we should fast-path here and re-use that boolean value?
  __ ToBoolean(object, check_type(), object_is_true, object_is_false, true);
  __ bind(*object_is_true);
  __ LoadRoot(return_value, RootIndex::kTrueValue);
  __ Jump(&done);
  __ bind(*object_is_false);
  __ LoadRoot(return_value, RootIndex::kFalseValue);
  __ bind(&done);
}

void ToBooleanLogicalNot::SetValueLocationConstraints() {
  UseRegister(value());
  DefineAsRegister(this);
}
void ToBooleanLogicalNot::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  Register object = ToRegister(value());
  Register return_value = ToRegister(result());
  Label done;
  ZoneLabelRef object_is_true(masm), object_is_false(masm);
  __ ToBoolean(object, check_type(), object_is_true, object_is_false, true);
  __ bind(*object_is_true);
  __ LoadRoot(return_value, RootIndex::kFalseValue);
  __ Jump(&done);
  __ bind(*object_is_false);
  __ LoadRoot(return_value, RootIndex::kTrueValue);
  __ bind(&done);
}

int ToName::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kToName>::type;
  return D::GetStackParameterCount();
}
void ToName::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kToName>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(value_input(), D::GetRegisterParameter(D::kInput));
  DefineAsFixed(this, kReturnRegister0);
}
void ToName::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {
  __ CallBuiltin<Builtin::kToName>(context(),     // context
                                   value_input()  // input
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int ToNumberOrNumeric::MaxCallStackArgs() const {
  return TypeConversionDescriptor::GetStackParameterCount();
}
void ToNumberOrNumeric::SetValueLocationConstraints() {
  UseRegister(value_input());
  set_temporaries_needed(1);
  DefineAsRegister(this);
}
void ToNumberOrNumeric::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  ZoneLabelRef done(masm);
  Label move_and_return;
  Register object = ToRegister(value_input());
  Register result_reg = ToRegister(result());

  __ JumpIfSmi(object, &move_and_return, Label::kNear);
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();
  __ CompareMapWithRoot(object, RootIndex::kHeapNumberMap, scratch);
  __ JumpToDeferredIf(
      kNotEqual,
      [](MaglevAssembler* masm, Object::Conversion mode, Register object,
         Register result_reg, ToNumberOrNumeric* node, ZoneLabelRef done) {
        {
          RegisterSnapshot snapshot = node->register_snapshot();
          snapshot.live_registers.clear(result_reg);
          SaveRegisterStateForCall save_register_state(masm, snapshot);
          switch (mode) {
            case Object::Conversion::kToNumber:
              __ CallBuiltin<Builtin::kToNumber>(
                  masm->native_context().object(), object);
              break;
            case Object::Conversion::kToNumeric:
              __ CallBuiltin<Builtin::kToNumeric>(
                  masm->native_context().object(), object);
              break;
          }
          masm->DefineExceptionHandlerPoint(node);
          save_register_state.DefineSafepointWithLazyDeopt(
              node->lazy_deopt_info());
          __ Move(result_reg, kReturnRegister0);
        }
        __ Jump(*done);
      },
      mode(), object, result_reg, this, done);
  __ bind(&move_and_return);
  __ Move(result_reg, object);

  __ bind(*done);
}

int ToObject::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kToObject>::type;
  return D::GetStackParameterCount();
}
void ToObject::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kToObject>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(value_input(), D::GetRegisterParameter(D::kInput));
  DefineAsFixed(this, kReturnRegister0);
}
void ToObject::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {
  Register value = ToRegister(value_input());
  Label call_builtin, done;
  // Avoid the builtin call if {value} is a JSReceiver.
  if (check_type() == CheckType::kOmitHeapObjectCheck) {
    __ AssertNotSmi(value);
  } else {
    __ JumpIfSmi(value, &call_builtin, Label::Distance::kNear);
  }
  __ JumpIfJSAnyIsNotPrimitive(value, &done, Label::Distance::kNear);
  __ bind(&call_builtin);
  __ CallBuiltin<Builtin::kToObject>(context(),     // context
                                     value_input()  // input
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
  __ bind(&done);
}

int ToString::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kToString>::type;
  return D::GetStackParameterCount();
}
void ToString::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kToString>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(value_input(), D::GetRegisterParameter(D::kO));
  DefineAsFixed(this, kReturnRegister0);
}
void ToString::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {
  Register value = ToRegister(value_input());
  Label call_builtin, done;
  // Avoid the builtin call if {value} is a string.
  __ JumpIfSmi(value, &call_builtin, Label::Distance::kNear);
  __ CompareObjectType(value, FIRST_NONSTRING_TYPE);
  __ JumpIf(kUnsignedLessThan, &done, Label::Distance::kNear);
  if (mode() == kConvertSymbol) {
    __ CompareObjectType(value, SYMBOL_TYPE);
    __ JumpIf(kNotEqual, &call_builtin, Label::Distance::kNear);
    __ Push(value);
    __ CallRuntime(Runtime::kSymbolDescriptiveString, 1);
    __ Jump(&done, Label::kNear);
  }
  __ bind(&call_builtin);
  __ CallBuiltin<Builtin::kToString>(context(),     // context
                                     value_input()  // input
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
  __ bind(&done);
}

void NumberToString::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kNumberToString>::type;
  UseFixed(value_input(), D::GetRegisterParameter(D::kInput));
  DefineAsFixed(this, kReturnRegister0);
}
void NumberToString::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  __ CallBuiltin<Builtin::kNumberToString>(value_input());
  masm->DefineLazyDeoptPoint(this->lazy_deopt_info());
}

int ThrowReferenceErrorIfHole::MaxCallStackArgs() const { return 1; }
void ThrowReferenceErrorIfHole::SetValueLocationConstraints() {
  UseAny(value());
}
void ThrowReferenceErrorIfHole::GenerateCode(MaglevAssembler* masm,
                                             const ProcessingState& state) {
  __ JumpToDeferredIf(
      __ IsRootConstant(value(), RootIndex::kTheHoleValue),
      [](MaglevAssembler* masm, ThrowReferenceErrorIfHole* node) {
        __ Move(kContextRegister, masm->native_context().object());
        __ Push(node->name().object());
        __ CallRuntime(Runtime::kThrowAccessedUninitializedVariable, 1);
        masm->DefineExceptionHandlerAndLazyDeoptPoint(node);
        __ Abort(AbortReason::kUnexpectedReturnFromThrow);
      },
      this);
}

int ThrowSuperNotCalledIfHole::MaxCallStackArgs() const { return 0; }
void ThrowSuperNotCalledIfHole::SetValueLocationConstraints() {
  UseAny(value());
}
void ThrowSuperNotCalledIfHole::GenerateCode(MaglevAssembler* masm,
                                             const ProcessingState& state) {
  __ JumpToDeferredIf(
      __ IsRootConstant(value(), RootIndex::kTheHoleValue),
      [](MaglevAssembler* masm, ThrowSuperNotCalledIfHole* node) {
        __ Move(kContextRegister, masm->native_context().object());
        __ CallRuntime(Runtime::kThrowSuperNotCalled, 0);
        masm->DefineExceptionHandlerAndLazyDeoptPoint(node);
        __ Abort(AbortReason::kUnexpectedReturnFromThrow);
      },
      this);
}

int ThrowSuperAlreadyCalledIfNotHole::MaxCallStackArgs() const { return 0; }
void ThrowSuperAlreadyCalledIfNotHole::SetValueLocationConstraints() {
  UseAny(value());
}
void ThrowSuperAlreadyCalledIfNotHole::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  __ JumpToDeferredIf(
      NegateCondition(__ IsRootConstant(value(), RootIndex::kTheHoleValue)),
      [](MaglevAssembler* masm, ThrowSuperAlreadyCalledIfNotHole* node) {
        __ Move(kContextRegister, masm->native_context().object());
        __ CallRuntime(Runtime::kThrowSuperAlreadyCalledError, 0);
        masm->DefineExceptionHandlerAndLazyDeoptPoint(node);
        __ Abort(AbortReason::kUnexpectedReturnFromThrow);
      },
      this);
}

int ThrowIfNotCallable::MaxCallStackArgs() const { return 1; }
void ThrowIfNotCallable::SetValueLocationConstraints() {
  UseRegister(value());
  set_temporaries_needed(1);
}
void ThrowIfNotCallable::GenerateCode(MaglevAssembler* masm,
                                      const ProcessingState& state) {
  Label* if_not_callable = __ MakeDeferredCode(
      [](MaglevAssembler* masm, ThrowIfNotCallable* node) {
        __ Push(node->value());
        __ Move(kContextRegister, masm->native_context().object());
        __ CallRuntime(Runtime::kThrowCalledNonCallable, 1);
        masm->DefineExceptionHandlerAndLazyDeoptPoint(node);
        __ Abort(AbortReason::kUnexpectedReturnFromThrow);
      },
      this);

  Register value_reg = ToRegister(value());
  __ JumpIfSmi(value_reg, if_not_callable);

  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();
  __ LoadMap(scratch, value_reg);
  static_assert(Map::kBitFieldOffsetEnd + 1 - Map::kBitFieldOffset == 1);
  __ LoadUnsignedField(scratch, FieldMemOperand(scratch, Map::kBitFieldOffset),
                       1);
  __ TestInt32AndJumpIfAllClear(scratch, Map::Bits1::IsCallableBit::kMask,
                                if_not_callable);
}

int ThrowIfNotSuperConstructor::MaxCallStackArgs() const { return 2; }
void ThrowIfNotSuperConstructor::SetValueLocationConstraints() {
  UseRegister(constructor());
  UseRegister(function());
  set_temporaries_needed(1);
}
void ThrowIfNotSuperConstructor::GenerateCode(MaglevAssembler* masm,
                                              const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();
  __ LoadMap(scratch, ToRegister(constructor()));
  static_assert(Map::kBitFieldOffsetEnd + 1 - Map::kBitFieldOffset == 1);
  __ LoadUnsignedField(scratch, FieldMemOperand(scratch, Map::kBitFieldOffset),
                       1);
  __ TestInt32AndJumpIfAllClear(
      scratch, Map::Bits1::IsConstructorBit::kMask,
      __ MakeDeferredCode(
          [](MaglevAssembler* masm, ThrowIfNotSuperConstructor* node) {
            __ Push(ToRegister(node->constructor()),
                    ToRegister(node->function()));
            __ Move(kContextRegister, masm->native_context().object());
            __ CallRuntime(Runtime::kThrowNotSuperConstructor, 2);
            masm->DefineExceptionHandlerAndLazyDeoptPoint(node);
            __ Abort(AbortReason::kUnexpectedReturnFromThrow);
          },
          this));
}

void TruncateUint32ToInt32::SetValueLocationConstraints() {
  UseRegister(input());
  DefineSameAsFirst(this);
}
void TruncateUint32ToInt32::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  // No code emitted -- as far as the machine is concerned, int32 is uint32.
  DCHECK_EQ(ToRegister(input()), ToRegister(result()));
}

void TruncateFloat64ToInt32::SetValueLocationConstraints() {
  UseRegister(input());
  DefineAsRegister(this);
}
void TruncateFloat64ToInt32::GenerateCode(MaglevAssembler* masm,
                                          const ProcessingState& state) {
  __ TruncateDoubleToInt32(ToRegister(result()), ToDoubleRegister(input()));
}

void CheckedTruncateFloat64ToInt32::SetValueLocationConstraints() {
  UseRegister(input());
  DefineAsRegister(this);
}
void CheckedTruncateFloat64ToInt32::GenerateCode(MaglevAssembler* masm,
                                                 const ProcessingState& state) {
  __ TryTruncateDoubleToInt32(
      ToRegister(result()), ToDoubleRegister(input()),
      __ GetDeoptLabel(this, DeoptimizeReason::kNotInt32));
}

void UnsafeTruncateFloat64ToInt32::SetValueLocationConstraints() {
  UseRegister(input());
  DefineAsRegister(this);
}
void UnsafeTruncateFloat64ToInt32::GenerateCode(MaglevAssembler* masm,
                                                const ProcessingState& state) {
#ifdef DEBUG
  Label fail, start;
  __ Jump(&start);
  __ bind(&fail);
  __ Abort(AbortReason::kFloat64IsNotAInt32);

  __ bind(&start);
  __ TryTruncateDoubleToInt32(ToRegister(result()), ToDoubleRegister(input()),
                              &fail);
#else
  // TODO(dmercadier): TruncateDoubleToInt32 does additional work when the
  // double doesn't fit in a 32-bit integer. This is not necessary for
  // UnsafeTruncateFloat64ToInt32 (since we statically know that it the double
  // fits in a 32-bit int) and could be instead just a Cvttsd2si (x64) or Fcvtzs
  // (arm64).
  __ TruncateDoubleToInt32(ToRegister(result()), ToDoubleRegister(input()));
#endif
}

void CheckedUint32ToInt32::SetValueLocationConstraints() {
  UseRegister(input());
  DefineSameAsFirst(this);
}
void CheckedUint32ToInt32::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  Register input_reg = ToRegister(input());
  Label* fail = __ GetDeoptLabel(this, DeoptimizeReason::kNotInt32);
  __ CompareInt32AndJumpIf(input_reg, 0, kLessThan, fail);
}

void UnsafeTruncateUint32ToInt32::SetValueLocationConstraints() {
  UseRegister(input());
  DefineSameAsFirst(this);
}
void UnsafeTruncateUint32ToInt32::GenerateCode(MaglevAssembler* masm,
                                               const ProcessingState& state) {
#ifdef DEBUG
  Register input_reg = ToRegister(input());
  Label success;
  __ CompareInt32AndJumpIf(input_reg, 0, kGreaterThanEqual, &success);
  __ Abort(AbortReason::kUint32IsNotAInt32);
  __ bind(&success);
#endif
  // No code emitted -- as far as the machine is concerned, int32 is uint32.
  DCHECK_EQ(ToRegister(input()), ToRegister(result()));
}

void Int32ToUint8Clamped::SetValueLocationConstraints() {
  UseRegister(input());
  DefineSameAsFirst(this);
}
void Int32ToUint8Clamped::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  Register value = ToRegister(input());
  Register result_reg = ToRegister(result());
  DCHECK_EQ(value, result_reg);
  Label min, done;
  __ CompareInt32(value, 0);
  __ JumpIf(kLessThanEqual, &min);
  __ CompareInt32(value, 255);
  __ JumpIf(kLessThanEqual, &done);
  __ Move(result_reg, 255);
  __ Jump(&done, Label::Distance::kNear);
  __ bind(&min);
  __ Move(result_reg, 0);
  __ bind(&done);
}

void Uint32ToUint8Clamped::SetValueLocationConstraints() {
  UseRegister(input());
  DefineSameAsFirst(this);
}
void Uint32ToUint8Clamped::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  Register value = ToRegister(input());
  DCHECK_EQ(value, ToRegister(result()));
  Label done;
  __ CompareInt32(value, 255);
  __ JumpIf(kUnsignedLessThanEqual, &done, Label::Distance::kNear);
  __ Move(value, 255);
  __ bind(&done);
}

void Float64ToUint8Clamped::SetValueLocationConstraints() {
  UseRegister(input());
  DefineAsRegister(this);
}
void Float64ToUint8Clamped::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  DoubleRegister value = ToDoubleRegister(input());
  Register result_reg = ToRegister(result());
  Label min, max, done;
  __ ToUint8Clamped(result_reg, value, &min, &max, &done);
  __ bind(&min);
  __ Move(result_reg, 0);
  __ Jump(&done, Label::Distance::kNear);
  __ bind(&max);
  __ Move(result_reg, 255);
  __ bind(&done);
}

void CheckedNumberToUint8Clamped::SetValueLocationConstraints() {
  UseRegister(input());
  DefineSameAsFirst(this);
  set_temporaries_needed(1);
  set_double_temporaries_needed(1);
}
void CheckedNumberToUint8Clamped::GenerateCode(MaglevAssembler* masm,
                                               const ProcessingState& state) {
  Register value = ToRegister(input());
  Register result_reg = ToRegister(result());
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();
  DoubleRegister double_value = temps.AcquireDouble();
  Label is_not_smi, min, max, done;
  // Check if Smi.
  __ JumpIfNotSmi(value, &is_not_smi);
  // If Smi, convert to Int32.
  __ SmiToInt32(value);
  // Clamp.
  __ CompareInt32(value, 0);
  __ JumpIf(kLessThanEqual, &min);
  __ CompareInt32(value, 255);
  __ JumpIf(kGreaterThanEqual, &max);
  __ Jump(&done);
  __ bind(&is_not_smi);
  // Check if HeapNumber, deopt otherwise.
  __ CompareMapWithRoot(value, RootIndex::kHeapNumberMap, scratch);
  __ EmitEagerDeoptIf(kNotEqual, DeoptimizeReason::kNotANumber, this);
  // If heap number, get double value.
  __ LoadHeapNumberValue(double_value, value);
  // Clamp.
  __ ToUint8Clamped(value, double_value, &min, &max, &done);
  __ bind(&min);
  __ Move(result_reg, 0);
  __ Jump(&done, Label::Distance::kNear);
  __ bind(&max);
  __ Move(result_reg, 255);
  __ bind(&done);
}

void StoreFixedArrayElementWithWriteBarrier::SetValueLocationConstraints() {
  UseRegister(elements_input());
  UseRegister(index_input());
  UseRegister(value_input());
  RequireSpecificTemporary(WriteBarrierDescriptor::ObjectRegister());
  RequireSpecificTemporary(WriteBarrierDescriptor::SlotAddressRegister());
}
void StoreFixedArrayElementWithWriteBarrier::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register elements = ToRegister(elements_input());
  Register index = ToRegister(index_input());
  Register value = ToRegister(value_input());
  __ StoreFixedArrayElementWithWriteBarrier(elements, index, value,
                                            register_snapshot());
}

void StoreFixedArrayElementNoWriteBarrier::SetValueLocationConstraints() {
  UseRegister(elements_input());
  UseRegister(index_input());
  UseRegister(value_input());
}
void StoreFixedArrayElementNoWriteBarrier::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register elements = ToRegister(elements_input());
  Register index = ToRegister(index_input());
  Register value = ToRegister(value_input());
  __ StoreFixedArrayElementNoWriteBarrier(elements, index, value);
}

// ---
// Arch agnostic call nodes
// ---

int Call::MaxCallStackArgs() const { return num_args(); }
void Call::SetValueLocationConstraints() {
  using D = CallTrampolineDescriptor;
  UseFixed(function(), D::GetRegisterParameter(D::kFunction));
  UseAny(arg(0));
  for (int i = 1; i < num_args(); i++) {
    UseAny(arg(i));
  }
  UseFixed(context(), kContextRegister);
  DefineAsFixed(this, kReturnRegister0);
}

void Call::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {
  __ PushReverse(base::make_iterator_range(args_begin(), args_end()));

  uint32_t arg_count = num_args();
  if (target_type_ == TargetType::kAny) {
    switch (receiver_mode_) {
      case ConvertReceiverMode::kNullOrUndefined:
        __ CallBuiltin<Builtin::kCall_ReceiverIsNullOrUndefined>(
            context(), function(), arg_count);
        break;
      case ConvertReceiverMode::kNotNullOrUndefined:
        __ CallBuiltin<Builtin::kCall_ReceiverIsNotNullOrUndefined>(
            context(), function(), arg_count);
        break;
      case ConvertReceiverMode::kAny:
        __ CallBuiltin<Builtin::kCall_ReceiverIsAny>(context(), function(),
                                                     arg_count);
        break;
    }
  } else {
    DCHECK_EQ(TargetType::kJSFunction, target_type_);
    switch (receiver_mode_) {
      case ConvertReceiverMode::kNullOrUndefined:
        __ CallBuiltin<Builtin::kCallFunction_ReceiverIsNullOrUndefined>(
            context(), function(), arg_count);
        break;
      case ConvertReceiverMode::kNotNullOrUndefined:
        __ CallBuiltin<Builtin::kCallFunction_ReceiverIsNotNullOrUndefined>(
            context(), function(), arg_count);
        break;
      case ConvertReceiverMode::kAny:
        __ CallBuiltin<Builtin::kCallFunction_ReceiverIsAny>(
            context(), function(), arg_count);
        break;
    }
  }

  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int CallSelf::MaxCallStackArgs() const {
  int actual_parameter_count = num_args() + 1;
  return std::max(expected_parameter_count_, actual_parameter_count);
}
void CallSelf::SetValueLocationConstraints() {
  UseAny(receiver());
  for (int i = 0; i < num_args(); i++) {
    UseAny(arg(i));
  }
  UseFixed(closure(), kJavaScriptCallTargetRegister);
  UseFixed(new_target(), kJavaScriptCallNewTargetRegister);
  UseFixed(context(), kContextRegister);
  DefineAsFixed(this, kReturnRegister0);
  set_temporaries_needed(1);
}

void CallSelf::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();
  int actual_parameter_count = num_args() + 1;
  if (actual_parameter_count < expected_parameter_count_) {
    int number_of_undefineds =
        expected_parameter_count_ - actual_parameter_count;
    __ LoadRoot(scratch, RootIndex::kUndefinedValue);
    __ PushReverse(receiver(),
                   base::make_iterator_range(args_begin(), args_end()),
                   RepeatValue(scratch, number_of_undefineds));
  } else {
    __ PushReverse(receiver(),
                   base::make_iterator_range(args_begin(), args_end()));
  }
  DCHECK_EQ(kContextRegister, ToRegister(context()));
  DCHECK_EQ(kJavaScriptCallTargetRegister, ToRegister(closure()));
  __ Move(kJavaScriptCallArgCountRegister, actual_parameter_count);
  DCHECK(!shared_function_info().HasBuiltinId());
  __ CallSelf();
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int CallKnownJSFunction::MaxCallStackArgs() const {
  int actual_parameter_count = num_args() + 1;
  return std::max(expected_parameter_count_, actual_parameter_count);
}
void CallKnownJSFunction::SetValueLocationConstraints() {
  UseAny(receiver());
  for (int i = 0; i < num_args(); i++) {
    UseAny(arg(i));
  }
  UseFixed(closure(), kJavaScriptCallTargetRegister);
  UseFixed(new_target(), kJavaScriptCallNewTargetRegister);
  UseFixed(context(), kContextRegister);
  DefineAsFixed(this, kReturnRegister0);
  set_temporaries_needed(1);
}

void CallKnownJSFunction::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();
  int actual_parameter_count = num_args() + 1;
  if (actual_parameter_count < expected_parameter_count_) {
    int number_of_undefineds =
        expected_parameter_count_ - actual_parameter_count;
    __ LoadRoot(scratch, RootIndex::kUndefinedValue);
    __ PushReverse(receiver(),
                   base::make_iterator_range(args_begin(), args_end()),
                   RepeatValue(scratch, number_of_undefineds));
  } else {
    __ PushReverse(receiver(),
                   base::make_iterator_range(args_begin(), args_end()));
  }
  // From here on, we're going to do a call, so all registers are valid temps,
  // except for the ones we're going to write. This is needed in case one of the
  // helper methods below wants to use a temp and one of these is in the temp
  // list (in particular, this can happen on arm64 where cp is a temp register
  // by default).
  temps.SetAvailable(MaglevAssembler::GetAllocatableRegisters() -
                     RegList{kContextRegister, kJavaScriptCallCodeStartRegister,
                             kJavaScriptCallTargetRegister,
                             kJavaScriptCallNewTargetRegister,
                             kJavaScriptCallArgCountRegister});
  DCHECK_EQ(kContextRegister, ToRegister(context()));
  DCHECK_EQ(kJavaScriptCallTargetRegister, ToRegister(closure()));
  __ Move(kJavaScriptCallArgCountRegister, actual_parameter_count);
  if (shared_function_info().HasBuiltinId()) {
    __ CallBuiltin(shared_function_info().builtin_id());
  } else {
    __ LoadTaggedField(kJavaScriptCallCodeStartRegister,
                       FieldMemOperand(kJavaScriptCallTargetRegister,
                                       JSFunction::kCodeOffset));
    __ CallCodeObject(kJavaScriptCallCodeStartRegister);
  }
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int CallBuiltin::MaxCallStackArgs() const {
  auto descriptor = Builtins::CallInterfaceDescriptorFor(builtin());
  if (!descriptor.AllowVarArgs()) {
    return descriptor.GetStackParameterCount();
  } else {
    int all_input_count = InputCountWithoutContext() + (has_feedback() ? 2 : 0);
    DCHECK_GE(all_input_count, descriptor.GetRegisterParameterCount());
    return all_input_count - descriptor.GetRegisterParameterCount();
  }
}

void CallBuiltin::SetValueLocationConstraints() {
  auto descriptor = Builtins::CallInterfaceDescriptorFor(builtin());
  bool has_context = descriptor.HasContextParameter();
  int i = 0;
  for (; i < InputsInRegisterCount(); i++) {
    UseFixed(input(i), descriptor.GetRegisterParameter(i));
  }
  for (; i < InputCountWithoutContext(); i++) {
    UseAny(input(i));
  }
  if (has_context) {
    UseFixed(input(i), kContextRegister);
  }
  DefineAsFixed(this, kReturnRegister0);
}

template <typename... Args>
void CallBuiltin::PushArguments(MaglevAssembler* masm, Args... extra_args) {
  auto descriptor = Builtins::CallInterfaceDescriptorFor(builtin());
  if (descriptor.GetStackArgumentOrder() == StackArgumentOrder::kDefault) {
    // In Default order we cannot have extra args (feedback).
    DCHECK_EQ(sizeof...(extra_args), 0);
    __ Push(base::make_iterator_range(stack_args_begin(), stack_args_end()));
  } else {
    DCHECK_EQ(descriptor.GetStackArgumentOrder(), StackArgumentOrder::kJS);
    __ PushReverse(extra_args..., base::make_iterator_range(stack_args_begin(),
                                                            stack_args_end()));
  }
}

void CallBuiltin::PassFeedbackSlotInRegister(MaglevAssembler* masm) {
  DCHECK(has_feedback());
  auto descriptor = Builtins::CallInterfaceDescriptorFor(builtin());
  int slot_index = InputCountWithoutContext();
  switch (slot_type()) {
    case kTaggedIndex:
      __ Move(descriptor.GetRegisterParameter(slot_index),
              TaggedIndex::FromIntptr(feedback().index()));
      break;
    case kSmi:
      __ Move(descriptor.GetRegisterParameter(slot_index),
              Smi::FromInt(feedback().index()));
      break;
  }
}

void CallBuiltin::PushFeedbackAndArguments(MaglevAssembler* masm) {
  DCHECK(has_feedback());

  auto descriptor = Builtins::CallInterfaceDescriptorFor(builtin());
  int slot_index = InputCountWithoutContext();
  int vector_index = slot_index + 1;

  // There are three possibilities:
  // 1. Feedback slot and vector are in register.
  // 2. Feedback slot is in register and vector is on stack.
  // 3. Feedback slot and vector are on stack.
  if (vector_index < descriptor.GetRegisterParameterCount()) {
    PassFeedbackSlotInRegister(masm);
    __ Move(descriptor.GetRegisterParameter(vector_index), feedback().vector);
    PushArguments(masm);
  } else if (vector_index == descriptor.GetRegisterParameterCount()) {
    PassFeedbackSlotInRegister(masm);
    DCHECK_EQ(descriptor.GetStackArgumentOrder(), StackArgumentOrder::kJS);
    // Ensure that the builtin only expects the feedback vector on the stack and
    // potentional additional var args are passed through to another builtin.
    // This is required to align the stack correctly (e.g. on arm64).
    DCHECK_EQ(descriptor.GetStackParameterCount(), 1);
    PushArguments(masm);
    __ Push(feedback().vector);
  } else {
    int slot = feedback().index();
    Handle<FeedbackVector> vector = feedback().vector;
    switch (slot_type()) {
      case kTaggedIndex:
        PushArguments(masm, TaggedIndex::FromIntptr(slot), vector);
        break;
      case kSmi:
        PushArguments(masm, Smi::FromInt(slot), vector);
        break;
    }
  }
}

void CallBuiltin::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  if (has_feedback()) {
    PushFeedbackAndArguments(masm);
  } else {
    PushArguments(masm);
  }
  __ CallBuiltin(builtin());
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int CallRuntime::MaxCallStackArgs() const { return num_args(); }
void CallRuntime::SetValueLocationConstraints() {
  UseFixed(context(), kContextRegister);
  for (int i = 0; i < num_args(); i++) {
    UseAny(arg(i));
  }
  DefineAsFixed(this, kReturnRegister0);
}
void CallRuntime::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  __ Push(base::make_iterator_range(args_begin(), args_end()));
  __ CallRuntime(function_id(), num_args());
  // TODO(victorgomes): Not sure if this is needed for all runtime calls.
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int CallWithSpread::MaxCallStackArgs() const {
  int argc_no_spread = num_args() - 1;
  using D = CallInterfaceDescriptorFor<Builtin::kCallWithSpread>::type;
  return argc_no_spread + D::GetStackParameterCount();
}
void CallWithSpread::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kCallWithSpread>::type;
  UseFixed(function(), D::GetRegisterParameter(D::kTarget));
  UseFixed(spread(), D::GetRegisterParameter(D::kSpread));
  UseFixed(context(), kContextRegister);
  for (int i = 0; i < num_args() - 1; i++) {
    UseAny(arg(i));
  }
  DefineAsFixed(this, kReturnRegister0);
}
void CallWithSpread::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  __ CallBuiltin<Builtin::kCallWithSpread>(
      context(),             // context
      function(),            // target
      num_args_no_spread(),  // arguments count
      spread(),              // spread
      base::make_iterator_range(args_no_spread_begin(),
                                args_no_spread_end())  // pushed args
  );

  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int CallWithArrayLike::MaxCallStackArgs() const {
  using D = CallInterfaceDescriptorFor<Builtin::kCallWithArrayLike>::type;
  return D::GetStackParameterCount();
}
void CallWithArrayLike::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<Builtin::kCallWithArrayLike>::type;
  UseFixed(function(), D::GetRegisterParameter(D::kTarget));
  UseAny(receiver());
  UseFixed(arguments_list(), D::GetRegisterParameter(D::kArgumentsList));
  UseFixed(context(), kContextRegister);
  DefineAsFixed(this, kReturnRegister0);
}
void CallWithArrayLike::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  // CallWithArrayLike is a weird builtin that expects a receiver as top of the
  // stack, but doesn't explicitly list it as an extra argument. Push it
  // manually, and assert that there are no other stack arguments.
  static_assert(
      CallInterfaceDescriptorFor<
          Builtin::kCallWithArrayLike>::type::GetStackParameterCount() == 0);
  __ Push(receiver());
  __ CallBuiltin<Builtin::kCallWithArrayLike>(
      context(),        // context
      function(),       // target
      arguments_list()  // arguments list
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

// ---
// Arch agnostic construct nodes
// ---

int Construct::MaxCallStackArgs() const {
  using D = Construct_WithFeedbackDescriptor;
  return num_args() + D::GetStackParameterCount();
}
void Construct::SetValueLocationConstraints() {
  using D = Construct_WithFeedbackDescriptor;
  UseFixed(function(), D::GetRegisterParameter(D::kTarget));
  UseFixed(new_target(), D::GetRegisterParameter(D::kNewTarget));
  UseFixed(context(), kContextRegister);
  for (int i = 0; i < num_args(); i++) {
    UseAny(arg(i));
  }
  DefineAsFixed(this, kReturnRegister0);
}
void Construct::GenerateCode(MaglevAssembler* masm,
                             const ProcessingState& state) {
  __ CallBuiltin<Builtin::kConstruct_WithFeedback>(
      context(),           // context
      function(),          // target
      new_target(),        // new target
      num_args(),          // actual arguments count
      feedback().index(),  // feedback slot
      feedback().vector,   // feedback vector
      base::make_iterator_range(args_begin(), args_end())  // args
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

int ConstructWithSpread::MaxCallStackArgs() const {
  int argc_no_spread = num_args() - 1;
  using D = CallInterfaceDescriptorFor<
      Builtin::kConstructWithSpread_WithFeedback>::type;
  return argc_no_spread + D::GetStackParameterCount();
}
void ConstructWithSpread::SetValueLocationConstraints() {
  using D = CallInterfaceDescriptorFor<
      Builtin::kConstructWithSpread_WithFeedback>::type;
  UseFixed(function(), D::GetRegisterParameter(D::kTarget));
  UseFixed(new_target(), D::GetRegisterParameter(D::kNewTarget));
  UseFixed(context(), kContextRegister);
  for (int i = 0; i < num_args() - 1; i++) {
    UseAny(arg(i));
  }
  UseFixed(spread(), D::GetRegisterParameter(D::kSpread));
  DefineAsFixed(this, kReturnRegister0);
}
void ConstructWithSpread::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  __ CallBuiltin<Builtin::kConstructWithSpread_WithFeedback>(
      context(),             // context
      function(),            // target
      new_target(),          // new target
      num_args_no_spread(),  // actual arguments count
      feedback().index(),    // feedback slot
      spread(),              // spread
      feedback().vector,     // feedback vector
      base::make_iterator_range(args_no_spread_begin(),
                                args_no_spread_end())  // args
  );
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void SetPendingMessage::SetValueLocationConstraints() {
  UseRegister(value());
  DefineAsRegister(this);
}

void SetPendingMessage::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  Register new_message = ToRegister(value());
  Register return_value = ToRegister(result());
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.GetDefaultScratchRegister();
  MemOperand pending_message_operand = __ ExternalReferenceAsOperand(
      ExternalReference::address_of_pending_message(masm->isolate()), scratch);
  if (new_message != return_value) {
    __ Move(return_value, pending_message_operand);
    __ Move(pending_message_operand, new_message);
  } else {
    __ Move(scratch, pending_message_operand);
    __ Move(pending_message_operand, new_message);
    __ Move(return_value, scratch);
  }
}

int TransitionElementsKindOrCheckMap::MaxCallStackArgs() const {
  return std::max(WriteBarrierDescriptor::GetStackParameterCount(), 2);
}

void TransitionElementsKindOrCheckMap::SetValueLocationConstraints() {
  UseRegister(object_input());
  set_temporaries_needed(1);
}

void TransitionElementsKindOrCheckMap::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register object = ToRegister(object_input());

  ZoneLabelRef done(masm);

  DCHECK(!AnyMapIsHeapNumber(transition_sources_));
  DCHECK(!transition_target_.object()->IsHeapNumberMap());

  if (check_type() == CheckType::kOmitHeapObjectCheck) {
    __ AssertNotSmi(object);
  } else {
    Condition is_smi = __ CheckSmi(object);
    __ EmitEagerDeoptIf(is_smi, DeoptimizeReason::kWrongMap, this);
  }

  Register map = temps.Acquire();
  __ LoadMap(map, object);

  for (compiler::MapRef transition_source : transition_sources_) {
    bool is_simple = IsSimpleMapChangeTransition(
        transition_source.elements_kind(), transition_target_.elements_kind());

    // TODO(leszeks): If there are a lot of transition source maps, move the
    // source into a register and share the deferred code between maps.
    __ CompareTagged(map, transition_source.object());
    // We can use `map` as a temporary register, since the deferred code will
    // jump to `done`, so we won't use it afterwards.
    __ JumpToDeferredIf(
        kEqual,
        [](MaglevAssembler* masm, Register object, Register temp,
           RegisterSnapshot register_snapshot,
           compiler::MapRef transition_target, bool is_simple,
           ZoneLabelRef done) {
          if (is_simple) {
            __ Move(temp, transition_target.object());
            __ StoreTaggedFieldWithWriteBarrier(
                object, HeapObject::kMapOffset, temp, register_snapshot,
                MaglevAssembler::kValueIsDecompressed,
                MaglevAssembler::kValueCannotBeSmi);
          } else {
            SaveRegisterStateForCall save_state(masm, register_snapshot);
            __ Push(object, transition_target.object());
            __ Move(kContextRegister, masm->native_context().object());
            __ CallRuntime(Runtime::kTransitionElementsKind);
            save_state.DefineSafepoint();
          }
          __ Jump(*done);
        },
        object, map, register_snapshot(), transition_target_, is_simple, done);
  }
  __ CompareTagged(map, transition_target_.object());
  __ EmitEagerDeoptIfNotEqual(DeoptimizeReason::kWrongMap, this);
  __ bind(*done);
}

namespace {

template <bool check_detached, typename ResultReg, typename NodeT>
void GenerateTypedArrayLoad(MaglevAssembler* masm, NodeT* node, Register object,
                            Register index, ResultReg result_reg,
                            ElementsKind kind) {
  __ AssertNotSmi(object);
  if (v8_flags.debug_code) {
    MaglevAssembler::ScratchRegisterScope temps(masm);
    __ IsObjectType(object, JS_TYPED_ARRAY_TYPE);
    __ Assert(kEqual, AbortReason::kUnexpectedValue);
  }

  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();

  if constexpr (check_detached) {
    __ DeoptIfBufferDetached(object, scratch, node);
  }

  Register data_pointer = scratch;
  __ BuildTypedArrayDataPointer(data_pointer, object);

  int element_size = ElementsKindSize(kind);
  MemOperand operand =
      __ TypedArrayElementOperand(data_pointer, index, element_size);
  if constexpr (std::is_same_v<ResultReg, Register>) {
    if (IsSignedIntTypedArrayElementsKind(kind)) {
      __ LoadSignedField(result_reg, operand, element_size);
    } else {
      DCHECK(IsUnsignedIntTypedArrayElementsKind(kind));
      __ LoadUnsignedField(result_reg, operand, element_size);
    }
  } else {
#ifdef DEBUG
    bool result_reg_is_double = std::is_same_v<ResultReg, DoubleRegister>;
    DCHECK(result_reg_is_double);
    DCHECK(IsFloatTypedArrayElementsKind(kind));
#endif
    switch (kind) {
      case FLOAT32_ELEMENTS:
        __ LoadFloat32(result_reg, operand);
        break;
      case FLOAT64_ELEMENTS:
        __ LoadFloat64(result_reg, operand);
        break;
      default:
        UNREACHABLE();
    }
  }
}

template <bool check_detached, typename ValueReg, typename NodeT>
void GenerateTypedArrayStore(MaglevAssembler* masm, NodeT* node,
                             Register object, Register index, ValueReg value,
                             ElementsKind kind) {
  __ AssertNotSmi(object);
  if (v8_flags.debug_code) {
    MaglevAssembler::ScratchRegisterScope temps(masm);
    __ IsObjectType(object, JS_TYPED_ARRAY_TYPE);
    __ Assert(kEqual, AbortReason::kUnexpectedValue);
  }

  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();

  if constexpr (check_detached) {
    __ DeoptIfBufferDetached(object, scratch, node);
  }

  Register data_pointer = scratch;
  __ BuildTypedArrayDataPointer(data_pointer, object);

  int element_size = ElementsKindSize(kind);
  MemOperand operand =
      __ TypedArrayElementOperand(data_pointer, index, element_size);
  if constexpr (std::is_same_v<ValueReg, Register>) {
    int element_size = ElementsKindSize(kind);
    __ StoreField(operand, value, element_size);
  } else {
#ifdef DEBUG
    bool value_is_double = std::is_same_v<ValueReg, DoubleRegister>;
    DCHECK(value_is_double);
    DCHECK(IsFloatTypedArrayElementsKind(kind));
#endif
    switch (kind) {
      case FLOAT32_ELEMENTS:
        __ StoreFloat32(operand, value);
        break;
      case FLOAT64_ELEMENTS:
        __ StoreFloat64(operand, value);
        break;
      default:
        UNREACHABLE();
    }
  }
}

}  // namespace

#define DEF_LOAD_TYPED_ARRAY(Name, ResultReg, ToResultReg, check_detached) \
  void Name::SetValueLocationConstraints() {                               \
    UseRegister(object_input());                                           \
    UseRegister(index_input());                                            \
    DefineAsRegister(this);                                                \
    set_temporaries_needed(1);                                             \
  }                                                                        \
  void Name::GenerateCode(MaglevAssembler* masm,                           \
                          const ProcessingState& state) {                  \
    Register object = ToRegister(object_input());                          \
    Register index = ToRegister(index_input());                            \
    ResultReg result_reg = ToResultReg(result());                          \
                                                                           \
    GenerateTypedArrayLoad<check_detached>(masm, this, object, index,      \
                                           result_reg, elements_kind_);    \
  }

DEF_LOAD_TYPED_ARRAY(LoadSignedIntTypedArrayElement, Register, ToRegister,
                     /*check_detached*/ true)
DEF_LOAD_TYPED_ARRAY(LoadSignedIntTypedArrayElementNoDeopt, Register,
                     ToRegister,
                     /*check_detached*/ false)

DEF_LOAD_TYPED_ARRAY(LoadUnsignedIntTypedArrayElement, Register, ToRegister,
                     /*check_detached*/ true)
DEF_LOAD_TYPED_ARRAY(LoadUnsignedIntTypedArrayElementNoDeopt, Register,
                     ToRegister,
                     /*check_detached*/ false)

DEF_LOAD_TYPED_ARRAY(LoadDoubleTypedArrayElement, DoubleRegister,
                     ToDoubleRegister,
                     /*check_detached*/ true)
DEF_LOAD_TYPED_ARRAY(LoadDoubleTypedArrayElementNoDeopt, DoubleRegister,
                     ToDoubleRegister, /*check_detached*/ false)
#undef DEF_LOAD_TYPED_ARRAY

#define DEF_STORE_TYPED_ARRAY(Name, ValueReg, ToValueReg, check_detached)     \
  void Name::SetValueLocationConstraints() {                                  \
    UseRegister(object_input());                                              \
    UseRegister(index_input());                                               \
    UseRegister(value_input());                                               \
    set_temporaries_needed(1);                                                \
  }                                                                           \
  void Name::GenerateCode(MaglevAssembler* masm,                              \
                          const ProcessingState& state) {                     \
    Register object = ToRegister(object_input());                             \
    Register index = ToRegister(index_input());                               \
    ValueReg value = ToValueReg(value_input());                               \
                                                                              \
    GenerateTypedArrayStore<check_detached>(masm, this, object, index, value, \
                                            elements_kind_);                  \
  }

DEF_STORE_TYPED_ARRAY(StoreIntTypedArrayElement, Register, ToRegister,
                      /*check_detached*/ true)
DEF_STORE_TYPED_ARRAY(StoreIntTypedArrayElementNoDeopt, Register, ToRegister,
                      /*check_detached*/ false)

DEF_STORE_TYPED_ARRAY(StoreDoubleTypedArrayElement, DoubleRegister,
                      ToDoubleRegister,
                      /*check_detached*/ true)
DEF_STORE_TYPED_ARRAY(StoreDoubleTypedArrayElementNoDeopt, DoubleRegister,
                      ToDoubleRegister, /*check_detached*/ false)

#undef DEF_STORE_TYPED_ARRAY

// ---
// Arch agnostic control nodes
// ---

void Jump::SetValueLocationConstraints() {}
void Jump::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {
  // Avoid emitting a jump to the next block.
  if (target() != state.next_block()) {
    __ Jump(target()->label());
  }
}

namespace {

void AttemptOnStackReplacement(MaglevAssembler* masm,
                               ZoneLabelRef no_code_for_osr,
                               TryOnStackReplacement* node, Register scratch0,
                               Register scratch1, int32_t loop_depth,
                               FeedbackSlot feedback_slot,
                               BytecodeOffset osr_offset) {
  // Two cases may cause us to attempt OSR, in the following order:
  //
  // 1) Presence of cached OSR Turbofan code.
  // 2) The OSR urgency exceeds the current loop depth - in that case, call
  //    into runtime to trigger a Turbofan OSR compilation. A non-zero return
  //    value means we should deopt into Ignition which will handle all further
  //    necessary steps (rewriting the stack frame, jumping to OSR'd code).
  //
  // See also: InterpreterAssembler::OnStackReplacement.

  __ AssertFeedbackVector(scratch0);

  // Case 1).
  Label deopt;
  Register maybe_target_code = scratch1;
  __ TryLoadOptimizedOsrCode(scratch1, CodeKind::TURBOFAN, scratch0,
                             feedback_slot, &deopt, Label::kFar);

  // Case 2).
  {
    __ LoadByte(scratch0,
                FieldMemOperand(scratch0, FeedbackVector::kOsrStateOffset));
    __ DecodeField<FeedbackVector::OsrUrgencyBits>(scratch0);
    __ JumpIfByte(kUnsignedLessThanEqual, scratch0, loop_depth,
                  *no_code_for_osr);

    // The osr_urgency exceeds the current loop_depth, signaling an OSR
    // request. Call into runtime to compile.
    {
      // At this point we need a custom register snapshot since additional
      // registers may be live at the eager deopt below (the normal
      // register_snapshot only contains live registers *after this
      // node*).
      // TODO(v8:7700): Consider making the snapshot location
      // configurable.
      RegisterSnapshot snapshot = node->register_snapshot();
      AddDeoptRegistersToSnapshot(&snapshot, node->eager_deopt_info());
      DCHECK(!snapshot.live_registers.has(maybe_target_code));
      SaveRegisterStateForCall save_register_state(masm, snapshot);
      if (node->unit()->is_inline()) {
        __ Push(Smi::FromInt(osr_offset.ToInt()), node->closure());
        __ Move(kContextRegister, masm->native_context().object());
        __ CallRuntime(Runtime::kCompileOptimizedOSRFromMaglevInlined, 2);
      } else {
        __ Push(Smi::FromInt(osr_offset.ToInt()));
        __ Move(kContextRegister, masm->native_context().object());
        __ CallRuntime(Runtime::kCompileOptimizedOSRFromMaglev, 1);
      }
      save_register_state.DefineSafepoint();
      __ Move(maybe_target_code, kReturnRegister0);
    }

    // A `0` return value means there is no OSR code available yet. Continue
    // execution in Maglev, OSR code will be picked up once it exists and is
    // cached on the feedback vector.
    __ Cmp(maybe_target_code, 0);
    __ JumpIf(kEqual, *no_code_for_osr);
  }

  __ bind(&deopt);
  if (V8_LIKELY(v8_flags.turbofan)) {
    // None of the mutated input registers should be a register input into the
    // eager deopt info.
    DCHECK_REGLIST_EMPTY(
        RegList{scratch0, scratch1} &
        GetGeneralRegistersUsedAsInputs(node->eager_deopt_info()));
    __ EmitEagerDeopt(node, DeoptimizeReason::kPrepareForOnStackReplacement);
  } else {
    // Continue execution in Maglev. With TF disabled we cannot OSR and thus it
    // doesn't make sense to start the process. We do still perform all
    // remaining bookkeeping above though, to keep Maglev code behavior roughly
    // the same in both configurations.
    __ Jump(*no_code_for_osr);
  }
}

}  // namespace

int TryOnStackReplacement::MaxCallStackArgs() const {
  // For the kCompileOptimizedOSRFromMaglev call.
  if (unit()->is_inline()) return 2;
  return 1;
}
void TryOnStackReplacement::SetValueLocationConstraints() {
  UseAny(closure());
  set_temporaries_needed(2);
}
void TryOnStackReplacement::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch0 = temps.Acquire();
  Register scratch1 = temps.Acquire();

  const Register osr_state = scratch1;
  __ Move(scratch0, unit_->feedback().object());
  __ AssertFeedbackVector(scratch0);
  __ LoadByte(osr_state,
              FieldMemOperand(scratch0, FeedbackVector::kOsrStateOffset));

  ZoneLabelRef no_code_for_osr(masm);

  if (masm->compilation_info()->toplevel_is_osr()) {
    // TODO(olivf) The maybe_has_optimized_code bit is guaranteed to be set
    // since we have optimized maglev code -- but are waiting for TF. Thus we
    // have to go into the slow case more than neccessary.
    __ DecodeField<FeedbackVector::OsrUrgencyBits>(osr_state);
  }

  // The quick initial OSR check. If it passes, we proceed on to more
  // expensive OSR logic.
  static_assert(FeedbackVector::MaybeHasOptimizedOsrCodeBit::encode(true) >
                FeedbackVector::kMaxOsrUrgency);
  __ CompareInt32(osr_state, loop_depth_);
  __ JumpToDeferredIf(kUnsignedGreaterThan, AttemptOnStackReplacement,
                      no_code_for_osr, this, scratch0, scratch1, loop_depth_,
                      feedback_slot_, osr_offset_);
  __ bind(*no_code_for_osr);
}

void JumpLoop::SetValueLocationConstraints() {}
void JumpLoop::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {
  __ Jump(target()->label());
}

void BranchIfRootConstant::SetValueLocationConstraints() {
  UseRegister(condition_input());
}
void BranchIfRootConstant::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  __ CompareRoot(ToRegister(condition_input()), root_index());
  __ Branch(ConditionFor(Operation::kEqual), if_true(), if_false(),
            state.next_block());
}

void BranchIfToBooleanTrue::SetValueLocationConstraints() {
  // TODO(victorgomes): consider using any input instead.
  UseRegister(condition_input());
}
void BranchIfToBooleanTrue::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  // BasicBlocks are zone allocated and so safe to be casted to ZoneLabelRef.
  ZoneLabelRef true_label =
      ZoneLabelRef::UnsafeFromLabelPointer(if_true()->label());
  ZoneLabelRef false_label =
      ZoneLabelRef::UnsafeFromLabelPointer(if_false()->label());
  bool fallthrough_when_true = (if_true() == state.next_block());
  __ ToBoolean(ToRegister(condition_input()), check_type(), true_label,
               false_label, fallthrough_when_true);
}

void BranchIfInt32ToBooleanTrue::SetValueLocationConstraints() {
  // TODO(victorgomes): consider using any input instead.
  UseRegister(condition_input());
}
void BranchIfInt32ToBooleanTrue::GenerateCode(MaglevAssembler* masm,
                                              const ProcessingState& state) {
  __ CompareInt32(ToRegister(condition_input()), 0);
  __ Branch(kNotEqual, if_true(), if_false(), state.next_block());
}

void BranchIfFloat64ToBooleanTrue::SetValueLocationConstraints() {
  UseRegister(condition_input());
  set_double_temporaries_needed(1);
}
void BranchIfFloat64ToBooleanTrue::GenerateCode(MaglevAssembler* masm,
                                                const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  DoubleRegister double_scratch = temps.AcquireDouble();

  __ Move(double_scratch, 0.0);
  __ CompareFloat64(ToDoubleRegister(condition_input()), double_scratch);
  __ JumpIf(ConditionForNaN(), if_false()->label());
  __ Branch(kEqual, if_false(), if_true(), state.next_block());
}

void BranchIfFloat64IsHole::SetValueLocationConstraints() {
  UseRegister(condition_input());
  set_temporaries_needed(1);
}
void BranchIfFloat64IsHole::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();
  DoubleRegister input = ToDoubleRegister(condition_input());
  // See MaglevAssembler::Branch.
  bool fallthrough_when_true = if_true() == state.next_block();
  bool fallthrough_when_false = if_false() == state.next_block();
  if (fallthrough_when_false) {
    if (fallthrough_when_true) {
      // If both paths are a fallthrough, do nothing.
      DCHECK_EQ(if_true(), if_false());
      return;
    }
    // Jump over the false block if true, otherwise fall through into it.
    __ JumpIfHoleNan(input, scratch, if_true()->label(), Label::kFar);
  } else {
    // Jump to the false block if true.
    __ JumpIfNotHoleNan(input, scratch, if_false()->label(), Label::kFar);
    // Jump to the true block if it's not the next block.
    if (!fallthrough_when_true) {
      __ Jump(if_true()->label(), Label::kFar);
    }
  }
}

void BranchIfFloat64Compare::SetValueLocationConstraints() {
  UseRegister(left_input());
  UseRegister(right_input());
}
void BranchIfFloat64Compare::GenerateCode(MaglevAssembler* masm,
                                          const ProcessingState& state) {
  DoubleRegister left = ToDoubleRegister(left_input());
  DoubleRegister right = ToDoubleRegister(right_input());
  __ CompareFloat64(left, right);
  __ JumpIf(ConditionForNaN(), if_false()->label());
  __ Branch(ConditionForFloat64(operation_), if_true(), if_false(),
            state.next_block());
}

void BranchIfReferenceCompare::SetValueLocationConstraints() {
  UseRegister(left_input());
  UseRegister(right_input());
}
void BranchIfReferenceCompare::GenerateCode(MaglevAssembler* masm,
                                            const ProcessingState& state) {
  Register left = ToRegister(left_input());
  Register right = ToRegister(right_input());
  __ CmpTagged(left, right);
  __ Branch(ConditionFor(operation_), if_true(), if_false(),
            state.next_block());
}

void BranchIfInt32Compare::SetValueLocationConstraints() {
  UseRegister(left_input());
  UseRegister(right_input());
}
void BranchIfInt32Compare::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  Register left = ToRegister(left_input());
  Register right = ToRegister(right_input());
  __ CompareInt32(left, right);
  __ Branch(ConditionFor(operation_), if_true(), if_false(),
            state.next_block());
}

void BranchIfUndefinedOrNull::SetValueLocationConstraints() {
  UseRegister(condition_input());
}
void BranchIfUndefinedOrNull::GenerateCode(MaglevAssembler* masm,
                                           const ProcessingState& state) {
  Register value = ToRegister(condition_input());
  __ JumpIfRoot(value, RootIndex::kUndefinedValue, if_true()->label());
  __ JumpIfRoot(value, RootIndex::kNullValue, if_true()->label());
  auto* next_block = state.next_block();
  if (if_false() != next_block) {
    __ Jump(if_false()->label());
  }
}

void BranchIfUndetectable::SetValueLocationConstraints() {
  UseRegister(condition_input());
  set_temporaries_needed(1);
}
void BranchIfUndetectable::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  Register value = ToRegister(condition_input());
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();

  auto* next_block = state.next_block();
  if (next_block == if_true() || next_block != if_false()) {
    __ JumpIfNotUndetectable(value, scratch, check_type(), if_false()->label());
    if (next_block != if_true()) {
      __ Jump(if_true()->label());
    }
  } else {
    __ JumpIfUndetectable(value, scratch, check_type(), if_true()->label());
  }
}

void TestUndetectable::SetValueLocationConstraints() {
  UseRegister(value());
  set_temporaries_needed(1);
  DefineAsRegister(this);
}
void TestUndetectable::GenerateCode(MaglevAssembler* masm,
                                    const ProcessingState& state) {
  Register object = ToRegister(value());
  Register return_value = ToRegister(result());
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();

  Label return_false, done;
  __ JumpIfNotUndetectable(object, scratch, check_type(), &return_false,
                           Label::kNear);

  __ LoadRoot(return_value, RootIndex::kTrueValue);
  __ Jump(&done, Label::kNear);

  __ bind(&return_false);
  __ LoadRoot(return_value, RootIndex::kFalseValue);

  __ bind(&done);
}

void BranchIfTypeOf::SetValueLocationConstraints() {
  UseRegister(value_input());
  // One temporary for TestTypeOf.
  set_temporaries_needed(1);
}
void BranchIfTypeOf::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  Register value = ToRegister(value_input());
  __ TestTypeOf(value, literal_, if_true()->label(), Label::kFar,
                if_true() == state.next_block(), if_false()->label(),
                Label::kFar, if_false() == state.next_block());
}

void BranchIfJSReceiver::SetValueLocationConstraints() {
  UseRegister(condition_input());
}
void BranchIfJSReceiver::GenerateCode(MaglevAssembler* masm,
                                      const ProcessingState& state) {
  Register value = ToRegister(condition_input());
  __ JumpIfSmi(value, if_false()->label());
  __ JumpIfJSAnyIsNotPrimitive(value, if_true()->label());
  __ jmp(if_false()->label());
}

void Switch::SetValueLocationConstraints() {
  UseAndClobberRegister(value());
  // TODO(victorgomes): Create a arch-agnostic scratch register scope.
  set_temporaries_needed(1);
}
void Switch::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {
  MaglevAssembler::ScratchRegisterScope temps(masm);
  Register scratch = temps.Acquire();
  std::unique_ptr<Label*[]> labels = std::make_unique<Label*[]>(size());
  for (int i = 0; i < size(); i++) {
    BasicBlock* block = (targets())[i].block_ptr();
    block->set_start_block_of_switch_case(true);
    labels[i] = block->label();
  }
  Register val = ToRegister(value());
  // Switch requires {val} (the switch's condition) to be 64-bit, but maglev
  // usually manipulates/creates 32-bit integers. We thus sign-extend {val} to
  // 64-bit to have the correct value for negative numbers.
  __ SignExtend32To64Bits(val, val);
  __ Switch(scratch, val, value_base(), labels.get(), size());
  if (has_fallthrough()) {
    DCHECK_EQ(fallthrough(), state.next_block());
  } else {
    __ Trap();
  }
}

// ---
// Print params
// ---

void ExternalConstant::PrintParams(std::ostream& os,
                                   MaglevGraphLabeller* graph_labeller) const {
  os << "(" << reference() << ")";
}

void SmiConstant::PrintParams(std::ostream& os,
                              MaglevGraphLabeller* graph_labeller) const {
  os << "(" << value() << ")";
}

void Int32Constant::PrintParams(std::ostream& os,
                                MaglevGraphLabeller* graph_labeller) const {
  os << "(" << value() << ")";
}

void Float64Constant::PrintParams(std::ostream& os,
                                  MaglevGraphLabeller* graph_labeller) const {
  if (value().is_nan()) {
    os << "(NaN [0x" << std::hex << value().get_bits() << std::dec << "]";
    if (value().is_hole_nan()) {
      os << ", the hole";
    } else if (value().get_bits() ==
               base::bit_cast<uint64_t>(
                   std::numeric_limits<double>::quiet_NaN())) {
      os << ", quiet NaN";
    }
    os << ")";

  } else {
    os << "(" << value().get_scalar() << ")";
  }
}

void Constant::PrintParams(std::ostream& os,
                           MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *object_.object() << ")";
}

void DeleteProperty::PrintParams(std::ostream& os,
                                 MaglevGraphLabeller* graph_labeller) const {
  os << "(" << LanguageMode2String(mode()) << ")";
}

void InitialValue::PrintParams(std::ostream& os,
                               MaglevGraphLabeller* graph_labeller) const {
  os << "(" << source().ToString() << ")";
}

void LoadGlobal::PrintParams(std::ostream& os,
                             MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *name().object() << ")";
}

void StoreGlobal::PrintParams(std::ostream& os,
                              MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *name().object() << ")";
}

void RegisterInput::PrintParams(std::ostream& os,
                                MaglevGraphLabeller* graph_labeller) const {
  os << "(" << input() << ")";
}

void RootConstant::PrintParams(std::ostream& os,
                               MaglevGraphLabeller* graph_labeller) const {
  os << "(" << RootsTable::name(index()) << ")";
}

void CreateFunctionContext::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *scope_info().object() << ", " << slot_count() << ")";
}

void FastCreateClosure::PrintParams(std::ostream& os,
                                    MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *shared_function_info().object() << ", "
     << feedback_cell().object() << ")";
}

void CreateClosure::PrintParams(std::ostream& os,
                                MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *shared_function_info().object() << ", "
     << feedback_cell().object();
  if (pretenured()) {
    os << " [pretenured]";
  }
  os << ")";
}

void AllocateRaw::PrintParams(std::ostream& os,
                              MaglevGraphLabeller* graph_labeller) const {
  os << "(" << allocation_type() << ", " << size() << ")";
}

void FoldedAllocation::PrintParams(std::ostream& os,
                                   MaglevGraphLabeller* graph_labeller) const {
  os << "(+" << offset() << ")";
}

void Abort::PrintParams(std::ostream& os,
                        MaglevGraphLabeller* graph_labeller) const {
  os << "(" << GetAbortReason(reason()) << ")";
}

void AssertInt32::PrintParams(std::ostream& os,
                              MaglevGraphLabeller* graph_labeller) const {
  os << "(" << condition_ << ")";
}

void BuiltinStringPrototypeCharCodeOrCodePointAt::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  switch (mode_) {
    case BuiltinStringPrototypeCharCodeOrCodePointAt::kCharCodeAt:
      os << "(CharCodeAt)";
      break;
    case BuiltinStringPrototypeCharCodeOrCodePointAt::kCodePointAt:
      os << "(CodePointAt)";
      break;
  }
}

void CheckMaps::PrintParams(std::ostream& os,
                            MaglevGraphLabeller* graph_labeller) const {
  os << "(";
  bool first = true;
  for (compiler::MapRef map : maps()) {
    if (first) {
      first = false;
    } else {
      os << ", ";
    }
    os << *map.object();
  }
  os << ")";
}

void CheckValue::PrintParams(std::ostream& os,
                             MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *value().object() << ")";
}

void CheckValueEqualsInt32::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << value() << ")";
}

void CheckValueEqualsFloat64::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << value() << ")";
}

void CheckValueEqualsString::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *value().object() << ")";
}

void CheckInstanceType::PrintParams(std::ostream& os,
                                    MaglevGraphLabeller* graph_labeller) const {
  os << "(" << first_instance_type_;
  if (first_instance_type_ != last_instance_type_) {
    os << " - " << last_instance_type_;
  }
  os << ")";
}

void CheckMapsWithMigration::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(";
  bool first = true;
  for (compiler::MapRef map : maps()) {
    if (first) {
      first = false;
    } else {
      os << ", ";
    }
    os << *map.object();
  }
  os << ")";
}

void CheckInt32Condition::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << condition() << ", " << reason() << ")";
}

void CheckedNumberOrOddballToFloat64::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << conversion_type() << ")";
}

void UncheckedNumberOrOddballToFloat64::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << conversion_type() << ")";
}

void CheckedTruncateNumberOrOddballToInt32::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << conversion_type() << ")";
}

void TruncateNumberOrOddballToInt32::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << conversion_type() << ")";
}

void LoadTaggedField::PrintParams(std::ostream& os,
                                  MaglevGraphLabeller* graph_labeller) const {
  os << "(0x" << std::hex << offset() << std::dec;
  // Print compression status only after the result is allocated, since that's
  // when we do decompression marking.
  if (!result().operand().IsUnallocated()) {
    if (decompresses_tagged_result()) {
      os << ", decompressed";
    } else {
      os << ", compressed";
    }
  }
  os << ")";
}

void LoadDoubleField::PrintParams(std::ostream& os,
                                  MaglevGraphLabeller* graph_labeller) const {
  os << "(0x" << std::hex << offset() << std::dec << ")";
}

void LoadFixedArrayElement::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  // Print compression status only after the result is allocated, since that's
  // when we do decompression marking.
  if (!result().operand().IsUnallocated()) {
    if (decompresses_tagged_result()) {
      os << "(decompressed)";
    } else {
      os << "(compressed)";
    }
  }
}

void StoreDoubleField::PrintParams(std::ostream& os,
                                   MaglevGraphLabeller* graph_labeller) const {
  os << "(0x" << std::hex << offset() << std::dec << ")";
}

void StoreFloat64::PrintParams(std::ostream& os,
                               MaglevGraphLabeller* graph_labeller) const {
  os << "(0x" << std::hex << offset() << std::dec << ")";
}

void StoreTaggedFieldNoWriteBarrier::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(0x" << std::hex << offset() << std::dec << ")";
}

void StoreMap::PrintParams(std::ostream& os,
                           MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *map_.object() << ")";
}

void StoreTaggedFieldWithWriteBarrier::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(0x" << std::hex << offset() << std::dec << ")";
}

void LoadNamedGeneric::PrintParams(std::ostream& os,
                                   MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *name_.object() << ")";
}

void LoadNamedFromSuperGeneric::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *name_.object() << ")";
}

void SetNamedGeneric::PrintParams(std::ostream& os,
                                  MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *name_.object() << ")";
}

void DefineNamedOwnGeneric::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *name_.object() << ")";
}

void HasInPrototypeChain::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *prototype_.object() << ")";
}

void GapMove::PrintParams(std::ostream& os,
                          MaglevGraphLabeller* graph_labeller) const {
  os << "(" << source() << " → " << target() << ")";
}

void ConstantGapMove::PrintParams(std::ostream& os,
                                  MaglevGraphLabeller* graph_labeller) const {
  os << "(";
  graph_labeller->PrintNodeLabel(os, node_);
  os << " → " << target() << ")";
}

void Float64Ieee754Unary::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "("
     << ExternalReferenceTable::NameOfIsolateIndependentAddress(
            ieee_function_.address())
     << ")";
}

void Float64Round::PrintParams(std::ostream& os,
                               MaglevGraphLabeller* graph_labeller) const {
  switch (kind_) {
    case Kind::kCeil:
      os << "(ceil)";
      return;
    case Kind::kFloor:
      os << "(floor)";
      return;
    case Kind::kNearest:
      os << "(nearest)";
      return;
  }
}

void Phi::PrintParams(std::ostream& os,
                      MaglevGraphLabeller* graph_labeller) const {
  os << "(" << owner().ToString() << ")";
}

void Call::PrintParams(std::ostream& os,
                       MaglevGraphLabeller* graph_labeller) const {
  os << "(" << receiver_mode_ << ", ";
  switch (target_type_) {
    case TargetType::kJSFunction:
      os << "JSFunction";
      break;
    case TargetType::kAny:
      os << "Any";
      break;
  }
  os << ")";
}

void CallSelf::PrintParams(std::ostream& os,
                           MaglevGraphLabeller* graph_labeller) const {
  os << "(" << shared_function_info_.object() << ")";
}

void CallKnownJSFunction::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << shared_function_info_.object() << ")";
}

void CallBuiltin::PrintParams(std::ostream& os,
                              MaglevGraphLabeller* graph_labeller) const {
  os << "(" << Builtins::name(builtin()) << ")";
}

void CallRuntime::PrintParams(std::ostream& os,
                              MaglevGraphLabeller* graph_labeller) const {
  os << "(" << Runtime::FunctionForId(function_id())->name << ")";
}

void TestTypeOf::PrintParams(std::ostream& os,
                             MaglevGraphLabeller* graph_labeller) const {
  os << "(" << interpreter::TestTypeOfFlags::ToString(literal_) << ")";
}

void ReduceInterruptBudgetForLoop::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << amount() << ")";
}

void ReduceInterruptBudgetForReturn::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << amount() << ")";
}

void Deopt::PrintParams(std::ostream& os,
                        MaglevGraphLabeller* graph_labeller) const {
  os << "(" << DeoptimizeReasonToString(reason()) << ")";
}

void BranchIfRootConstant::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << RootsTable::name(root_index_) << ")";
}

void BranchIfFloat64Compare::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << operation_ << ")";
}

void BranchIfInt32Compare::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << operation_ << ")";
}

void BranchIfReferenceCompare::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << operation_ << ")";
}

void BranchIfTypeOf::PrintParams(std::ostream& os,
                                 MaglevGraphLabeller* graph_labeller) const {
  os << "(" << interpreter::TestTypeOfFlags::ToString(literal_) << ")";
}

}  // namespace maglev
}  // namespace internal
}  // namespace v8
