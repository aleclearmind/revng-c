//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "revng/ADT/FilteredGraphTraits.h"
#include "revng/ADT/RecursiveCoroutine.h"
#include "revng/Model/Binary.h"
#include "revng/Model/IRHelpers.h"
#include "revng/Model/Type.h"
#include "revng/Model/VerifyHelper.h"
#include "revng/Support/Assert.h"
#include "revng/Support/Debug.h"

#include "revng-c/DataLayoutAnalysis/DLATypeSystem.h"
#include "revng-c/Support/FunctionTags.h"
#include "revng-c/Support/IRHelpers.h"
#include "revng-c/Support/ModelHelpers.h"

#include "../FuncOrCallInst.h"
#include "DLAMakeModelTypes.h"

using namespace model;
using namespace dla;

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

static Logger<> Log("dla-update-model-funcs");
static Logger<> ModelLog("dla-dump-model-with-funcs");

using model::PrimitiveTypeKind::Generic;
using model::PrimitiveTypeKind::PointerOrNumber;

/// Returns true if \a T can be upgraded by DLA
// TODO: implement a better way to merge qualified types, based on the lattice
// of primitive types.
static bool canBeUpgraded(QualifiedType T) {
  return T.isPrimitive(PointerOrNumber) or T.isPrimitive(Generic);
}

static const llvm::Value *toLLVMValue(const llvm::Argument &A) {
  return static_cast<const llvm::Value *>(&A);
}

static const llvm::Value *toLLVMValue(const llvm::Use &A) {
  return A.get();
}

/// If the DLA recovered a more precise type than the already existing one
/// for any of the arguments of a RawFunctionType, update the model accordingly.
template<typename T>
  requires std::same_as<std::remove_const_t<T>, llvm::CallInst>
           or std::same_as<std::remove_const_t<T>, llvm::Function>
static bool updateArgumentTypes(model::Binary &Model,
                                RawFunctionType *ModelPrototype,
                                const T *CallOrFunction,
                                const TypeMapT &DLATypes) {
  bool Updated = false;
  auto &ModelArgs = ModelPrototype->Arguments();
  auto LLVMArgs = getArgs(CallOrFunction);

  uint64_t EffectiveLLVMArgSize = arg_size(CallOrFunction);

  // In case of presence of stack arguments, there's an extra argument
  if (not ModelPrototype->StackArgumentsType().empty()) {
    revng_log(Log, "Updating stack argument");
    auto *ModelStackArg = ModelPrototype->StackArgumentsType().get();
    revng_assert(not LLVMArgs.empty());

    // If the ModelStackArgs is an empty struct we have to fill it up, otherwise
    // it's already known and we don't have to mess it up.
    auto *ModelStackStruct = cast<model::StructType>(ModelStackArg);
    if (ModelStackStruct->Fields().empty()) {
      revng_assert(ModelStackStruct->size());
      auto ModelStackSize = *ModelStackStruct->size();

      // The stack argument on llvm IR is the last argument
      const llvm::Value *LLVMStackArg = toLLVMValue(*std::prev(LLVMArgs.end()));

      revng_log(Log,
                "Updating stack arg " << LLVMStackArg->getNameOrAsOperand());
      LayoutTypePtr Key{ LLVMStackArg, LayoutTypePtr::fieldNumNone };

      if (auto NewTypeIt = DLATypes.find(Key); NewTypeIt != DLATypes.end()) {
        using namespace model;
        QualifiedType DLAStackType = NewTypeIt->second;
        revng_assert(DLAStackType.size());
        auto DLAStackSize = *DLAStackType.size();

        Type *UnqualifiedType = DLAStackType.UnqualifiedType().get();
        auto *DLAStackStruct = dyn_cast<StructType>(UnqualifiedType);
        if (DLAStackType.Qualifiers().empty() and DLAStackStruct != nullptr) {
          // Import all the fields of the DLA struct that fit in
          for (auto &Field : DLAStackStruct->Fields()) {
            revng_assert(Field.Type().size());
            if (Field.Offset() + *Field.Type().size() > ModelStackSize)
              break;
            ModelStackStruct->Fields().insert(Field);
          }
          revng_log(Log, "Updated fields");
        } else if (DLAStackSize <= ModelStackSize) {
          // Insert a field of the type recovered by DLA at offset 0
          ModelStackStruct->Fields()[0].Type() = DLAStackType;
          revng_log(Log,
                    "Updated: inserted fields at offset 0 with ID: "
                      << UnqualifiedType->ID());
        } else {
          revng_log(Log, "Couldn't import the type into the model");
        }

        Updated = true;
      }
    }

    // Decrease the effective argument size, so the next loop leaves the stack
    // arguments alone.
    EffectiveLLVMArgSize -= 1;
  }

  revng_assert(ModelArgs.size() == EffectiveLLVMArgSize
               or (ModelArgs.size() == 1 and ModelArgs.begin()->Type().isVoid()
                   and EffectiveLLVMArgSize == 0));

  revng_log(Log, "Updating register arguments");

  for (const auto &[ModelArg, LLVMArg] : llvm::zip_first(ModelArgs, LLVMArgs)) {
    revng_assert(ModelArg.Type().isScalar());

    const llvm::Value *LLVMVal = toLLVMValue(LLVMArg);
    revng_log(Log, "Updating argument " << LLVMVal->getNameOrAsOperand());

    // Don't update if the type is already fine-grained or if the DLA has
    // nothing to say.
    // The latter situation can happen e.g. with unused variables, that have
    // no accesses in the TypeSystem graph. These nodes are pruned away in the
    // middle-end, therefore there is no Type associated to them at this
    // stage.
    if (canBeUpgraded(ModelArg.Type())) {
      LayoutTypePtr Key{ LLVMVal, LayoutTypePtr::fieldNumNone };
      if (auto NewTypeIt = DLATypes.find(Key); NewTypeIt != DLATypes.end()) {
        auto OldSize = *ModelArg.Type().size();
        // The type is associated to a LayoutTypeSystemPtr, hence we have to add
        // the pointer qualifier
        ModelArg.Type() = NewTypeIt->second.getPointerTo(Model.Architecture());
        revng_assert(*ModelArg.Type().size() == OldSize);
        revng_log(Log,
                  "Updated to "
                    << ModelArg.Type().UnqualifiedType().get()->ID());
        Updated = true;
      }
    }
  }

  return Updated;
}

/// If the DLA recovered a more precise type than the already existing one
/// for the return type of a RawFunctionType, update the model accordingly.
static bool updateReturnType(model::Binary &Model,
                             RawFunctionType *ModelPrototype,
                             const llvm::Value *LLVMRetVal,
                             const llvm::Type *LLVMRetType,
                             const TypeMapT &TypeMap) {
  bool Updated = false;
  auto &ModelRetVals = ModelPrototype->ReturnValues();
  revng_log(Log, "Updating return values");

  if (LLVMRetType->isIntOrPtrTy()) {
    revng_assert(ModelRetVals.size() == 1);
  } else if (LLVMRetType->isVoidTy()) {
    revng_assert(ModelRetVals.size() == 0);
  } else {
    const auto *RetValStruct = llvm::cast<llvm::StructType>(LLVMRetType);
    const auto &SubTypes = RetValStruct->subtypes();
    revng_assert(SubTypes.size() == ModelRetVals.size());

    const auto IsScalar = [](const llvm::Type *T) { return T->isIntOrPtrTy(); };
    revng_assert(llvm::all_of(SubTypes, IsScalar));
  }

  const bool IsScalar = ModelRetVals.size() == 1;
  for (auto &ModelRet : llvm::enumerate(ModelRetVals)) {

    unsigned int Index = IsScalar ? LayoutTypePtr::fieldNumNone :
                                    ModelRet.index();
    revng_log(Log,
              "Updating elem " << Index << " of "
                               << LLVMRetVal->getNameOrAsOperand());

    // Don't update if the type is already fine-grained or if the DLA has
    // nothing to say.
    // The latter situation can happen e.g. with unused variables, that have
    // no accesses in the TypeSystem graph. These nodes are pruned away in the
    // middle-end, therefore there is no Type associated to them at this
    // stage.
    auto &ModelReturnType = ModelRet.value().Type();
    if (canBeUpgraded(ModelReturnType)) {
      LayoutTypePtr Key{ LLVMRetVal, Index };
      if (auto NewTypeIt = TypeMap.find(Key); NewTypeIt != TypeMap.end()) {
        auto OldSize = ModelReturnType.size();
        // The type is associated to a LayoutTypeSystemPtr, hence we have to
        // add the pointer qualifier
        ModelReturnType = NewTypeIt->second.getPointerTo(Model.Architecture());
        revng_assert(ModelReturnType.size() == OldSize);
        revng_log(Log,
                  "Updated to "
                    << ModelReturnType.UnqualifiedType().get()->ID());
        Updated = true;
      }
    }
  }

  return Updated;
}

/// If the DLA recovered a more precise type than the already existing one
/// for any of the arguments of a CABIFunctionType, update the model
/// accordingly.
template<typename T>
  requires std::same_as<std::remove_const_t<T>, llvm::CallInst>
           or std::same_as<std::remove_const_t<T>, llvm::Function>
static bool updateArgumentTypes(model::Binary &Model,
                                CABIFunctionType *ModelPrototype,
                                const T *CallOrFunction,
                                const TypeMapT &DLATypes) {
  bool Updated = false;
  auto &ModelArgs = ModelPrototype->Arguments();
  auto LLVMArgs = getArgs(CallOrFunction);

  uint64_t EffectiveLLVMArgSize = arg_size(CallOrFunction);
  revng_assert(ModelArgs.size() == EffectiveLLVMArgSize
               or (ModelArgs.size() == 1 and ModelArgs.begin()->Type().isVoid()
                   and EffectiveLLVMArgSize == 0));

  revng_log(Log, "Updating arguments");

  for (const auto &[LLVMArg, ModelArg] : llvm::zip_first(LLVMArgs, ModelArgs)) {
    const llvm::Value *LLVMVal = toLLVMValue(LLVMArg);
    revng_log(Log, "Updating argument " << LLVMVal->getNameOrAsOperand());

    // Don't update if the type is already fine-grained or if the DLA has
    // nothing to say.
    // The latter situation can happen e.g. with unused variables, that have
    // no accesses in the TypeSystem graph. These nodes are pruned away in the
    // middle-end, therefore there is no Type associated to them at this
    // stage.
    if (canBeUpgraded(ModelArg.Type())) {
      LayoutTypePtr Key{ LLVMVal, LayoutTypePtr::fieldNumNone };
      if (auto NewTypeIt = DLATypes.find(Key); NewTypeIt != DLATypes.end()) {
        auto OldSize = *ModelArg.Type().size();
        // The type is associated to a LayoutTypeSystemPtr, hence we have to add
        // the pointer qualifier
        ModelArg.Type() = NewTypeIt->second.getPointerTo(Model.Architecture());
        revng_assert(*ModelArg.Type().size() == OldSize);
        revng_log(Log,
                  "Updated to "
                    << ModelArg.Type().UnqualifiedType().get()->ID());
        Updated = true;
      }
    }
  }

  return Updated;
}

/// If the DLA recovered a more precise type than the already existing one
/// for the return type of a CABIFunctionType, update the model accordingly.
static bool updateReturnType(model::Binary &Model,
                             CABIFunctionType *ModelPrototype,
                             const llvm::Value *LLVMRetVal,
                             const llvm::Type *LLVMRetType,
                             const TypeMapT &TypeMap) {
  auto &ModelReturnType = ModelPrototype->ReturnType();
  revng_log(Log, "Updating return value");

  // If the return type is void there's nothing to do.
  if (LLVMRetType->isVoidTy()) {
    revng_log(Log, "Is void");
    revng_assert(not ModelReturnType.UnqualifiedType().empty()
                 or ModelReturnType.isVoid());
    return false;
  }

  if (ModelReturnType.isScalar()) {
    if (!LLVMRetType->isIntOrPtrTy()) {
      revng_log(Log,
                "WARNING: model::CABIFunctionType returns a scalar type, the "
                "associated llvm::Function should return an integer or a "
                "pointer: "
                  << LLVMRetVal->getNameOrAsOperand());
      // If this happens we have an aggregate on LLVMIR and a scalar on
      // CABIFunction type. This should only happen in corner cases and we don't
      // have a general way to solve it properly right now, so we bail out and
      // never update that.
      return false;
    }

    revng_log(Log, "Is scalar: " << LLVMRetVal->getNameOrAsOperand());
    revng_assert(ModelReturnType.isScalar());

    // Don't update if the type is already fine-grained or if the DLA has
    // nothing to say.
    // The latter situation can happen e.g. with unused variables, that have
    // no accesses in the TypeSystem graph. These nodes are pruned away in the
    // middle-end, therefore there is no Type associated to them at this
    // stage.
    if (canBeUpgraded(ModelReturnType)) {
      LayoutTypePtr Key{ LLVMRetVal, LayoutTypePtr::fieldNumNone };
      if (auto NewTypeIt = TypeMap.find(Key); NewTypeIt != TypeMap.end()) {
        auto OldSize = *ModelReturnType.size();
        // The type is associated to a LayoutTypeSystemPtr, hence we have to
        // add the pointer qualifier
        ModelReturnType = NewTypeIt->second.getPointerTo(Model.Architecture());
        revng_assert(ModelReturnType.size() == OldSize);
        revng_log(Log,
                  "Updated to "
                    << ModelReturnType.UnqualifiedType().get()->ID());
        return true;
      }
    }
    return false;
  }

  // The case where CABIFunctionType's return type is scalar (meaning that
  // the corresponding IR also mentions a scalar) is handled above, as for
  // the aggregate case, we decided to just not do anything.
  //
  // The main reason behind such approach is the fact that usually non-scalar
  // return value types come from the places we should not override anyway,
  // i.e. debug information in the binary or the user.
  //
  // Also note, that big aggregate return value types are usually handled with
  // a shadow pointer argument, and so they are already scalars (pointers)
  // in the case where we deduce them manually.
  return false;
}

/// Update the prototype of a model::Function or of a call site with the types
/// recovered by DLA.
template<typename T>
  requires std::same_as<std::remove_const_t<T>, llvm::CallInst>
           or std::same_as<std::remove_const_t<T>, llvm::Function>
static bool updatePrototype(model::Binary &Model,
                            model::Type *Prototype,
                            const T *CallOrFunction,
                            const TypeMapT &TypeMap) {

  revng_assert(Prototype);
  bool Updated = false;

  revng_log(Log, "Updating func prototype");

  if (auto *RawPrototype = dyn_cast<RawFunctionType>(Prototype)) {
    Updated |= updateArgumentTypes(Model,
                                   RawPrototype,
                                   CallOrFunction,
                                   TypeMap);
    Updated |= updateReturnType(Model,
                                RawPrototype,
                                CallOrFunction,
                                getRetType(CallOrFunction),
                                TypeMap);
  } else if (auto *CABIPrototype = dyn_cast<CABIFunctionType>(Prototype)) {
    Updated |= updateArgumentTypes(Model,
                                   CABIPrototype,
                                   CallOrFunction,
                                   TypeMap);
    Updated |= updateReturnType(Model,
                                CABIPrototype,
                                CallOrFunction,
                                getRetType(CallOrFunction),
                                TypeMap);
  } else {
    revng_abort("Unsupported function type");
  }

  return Updated;
}

static RecursiveCoroutine<void>
fillStructWithRecoveredDLATypeAtOffset(model::Binary &Model,
                                       model::StructType *OriginalStruct,
                                       model::QualifiedType RecoveredType,
                                       uint64_t Offset = 0ULL) {
  uint64_t RecoveredSize = RecoveredType.size().value_or(0ULL);
  revng_assert(RecoveredSize > 0);

  uint64_t OriginalStructSize = OriginalStruct->size().value_or(0ULL);
  revng_assert(OriginalStructSize > 0);

  bool IsPointerOrArray = RecoveredType.isArray() or RecoveredType.isPointer();
  auto *RecoveredUnqualType = RecoveredType.UnqualifiedType().get();

  if (IsPointerOrArray or isa<model::PrimitiveType>(RecoveredUnqualType)
      or isa<model::EnumType>(RecoveredUnqualType)) {

    revng_assert(RecoveredSize + Offset <= OriginalStructSize);

    auto &OriginalFields = OriginalStruct->Fields();
    auto OriginalFieldsBegin = OriginalFields.begin();
    auto OriginalFieldsEnd = OriginalFields.end();

    auto FieldAtHigherOffsetIt = OriginalFields.upper_bound(Offset);
    if (FieldAtHigherOffsetIt != OriginalFieldsEnd) {
      uint64_t HigherOffset = FieldAtHigherOffsetIt->Offset();
      revng_assert(HigherOffset > Offset);
      // If the RecoveredType ends after the *FieldAtHigherOffsetIt starts, they
      // overlap, so we have to bail out.
      if (Offset + RecoveredSize > HigherOffset)
        rc_return;

      // Otherwise we can inject RecoveredType directly in OriginalStruct at
      // Offset. For that we don't need to change neither NewOffset, nor
      // TargetOriginalFieldIt.
    }

    auto FieldAtGTEOffsetIt = FieldAtHigherOffsetIt == OriginalFieldsBegin ?
                                OriginalFieldsEnd :
                                --FieldAtHigherOffsetIt;

    auto TargetOriginalFieldIt = OriginalFieldsEnd;
    uint64_t NewOffset = Offset;

    // If there is a field in OriginalStruct at lower offset than Offset, we
    // have to see if RecoveredType fits entirely into it, partially overlaps,
    // or is entirely past the end.
    if (FieldAtGTEOffsetIt != OriginalFieldsEnd) {

      uint64_t LowerOffset = FieldAtGTEOffsetIt->Offset();
      revng_assert(Offset >= LowerOffset);

      model::QualifiedType TypeAtLowerOffset = FieldAtGTEOffsetIt->Type();
      uint64_t FieldAtLowerOffsetSize = *TypeAtLowerOffset.size();

      if (Offset < LowerOffset + FieldAtLowerOffsetSize) {
        // Here RecoveredType starts inside *FieldAtGTEOffsetIt.
        // If RecoveredType ends after the end of *FieldAtGTEOffsetIt we bail
        // out, because we can't do anything sensible.
        if (Offset + RecoveredSize > LowerOffset + FieldAtLowerOffsetSize)
          rc_return;

        // Otherwise we have found a candidate target field. If the candidate
        // target field is a struct, we can compute the NewOffset, otherwise we
        // bail out, because if it's not a StructType we cannot inject anything.
        if (not TypeAtLowerOffset.is(model::TypeKind::StructType))
          rc_return;

        TargetOriginalFieldIt = FieldAtGTEOffsetIt;
        NewOffset -= LowerOffset;
      }
    }

    // If we reach this point without bailing out it means that we can inject
    // the field somewhere, either in OriginalStruct or in
    // *TargetOriginalFieldIt.

    // If we haven't found TargetOriginalFieldIt it means that the RecoveredType
    // can be injected directly as a field in OriginalStruct at Offset.
    if (TargetOriginalFieldIt == OriginalFieldsEnd) {
      revng_assert(Offset == NewOffset);
      OriginalStruct->Fields()[Offset].Type() = RecoveredType;
      rc_return;
    }

    // Otherwise we have to insert RecoveredType in *TargetOriginalFieldIt at
    // NewOffset.
    model::QualifiedType OriginalFieldType = TargetOriginalFieldIt->Type();
    model::Type *TargetFieldType = peelConstAndTypedefs(OriginalFieldType)
                                     .UnqualifiedType()
                                     .get();
    auto *TargetFieldStruct = cast<model::StructType>(TargetFieldType);

    uint64_t OldFieldSize = *TargetFieldStruct->size();
    rc_recur fillStructWithRecoveredDLATypeAtOffset(Model,
                                                    TargetFieldStruct,
                                                    RecoveredType,
                                                    NewOffset);

    revng_log(Log, "Updated field StructType: " << TargetFieldStruct->ID());
    revng_assert(*TargetFieldStruct->size() == OldFieldSize);
    revng_assert(TargetFieldStruct->verify());

    rc_return;
  }

  if (auto *NewS = dyn_cast<model::StructType>(RecoveredUnqualType)) {

    auto OriginalFieldsIt = OriginalStruct->Fields().begin();
    auto OriginalFieldsEnd = OriginalStruct->Fields().end();
    auto NewFieldsIt = NewS->Fields().begin();
    auto NewFieldsEnd = NewS->Fields().end();

    std::set<model::StructField *> CompatibleFields;
    while (NewFieldsIt != NewFieldsEnd) {

      // If a fields starts at the end, or after it, it's not compatible, nor is
      // any of the following fields.
      uint64_t NewStart = NewFieldsIt->Offset();
      if (NewStart >= OriginalStructSize)
        break;

      // If a fields ends after the end it's not compatible, nor is any of the
      // following fields.
      model::QualifiedType &NewFieldType = NewFieldsIt->Type();
      uint64_t NewEnd = NewFieldsIt->Offset() + *NewFieldType.size();
      revng_assert(NewStart < NewEnd);
      if (NewEnd > OriginalStructSize)
        break;

      // If we've reached the end of the original fields, the field is
      // compatible, and we can compare the next new field.
      if (OriginalFieldsIt == OriginalFieldsEnd) {
        CompatibleFields.insert(&*NewFieldsIt);
        ++NewFieldsIt;
        continue;
      }

      // If the new field ends before the start of the old field, it's
      // definitely compatible, and we can check the next new field.
      uint64_t OriginalStart = OriginalFieldsIt->Offset();
      if (NewEnd <= OriginalStart) {
        CompatibleFields.insert(&*NewFieldsIt);
        ++NewFieldsIt;
        continue;
      }

      // If we reach this point we don't know yet if its compatible or not.
      // First we have to make sure that we're comparing it against a meaningful
      // original field.

      // If the original field we're looking at is too far back, meaning that it
      // ends before this new field starts, the comparison is not meaningful,
      // because the two don't overlap but the new field could overlap with a
      // subsequent original field.
      // So we have to skip to the next original field.
      uint64_t OriginalEnd = OriginalFieldsIt->Offset()
                             + *OriginalFieldsIt->Type().size();
      revng_assert(OriginalStart < OriginalEnd);
      if (OriginalEnd <= NewStart) {
        ++OriginalFieldsIt;
        continue;
      }

      // If we reach this point, the new field is definitely overlapping.
      // Overlapping doesn't necessarily means incompatible though: the new
      // field might fall entirely inside one of the existing original fields.
      // If that's the case, and the original field has a struct type, we recur.
      if (OriginalStart <= NewStart and NewEnd <= OriginalEnd) {
        model::QualifiedType &OriginalFieldType = OriginalFieldsIt->Type();

        if (OriginalFieldType.is(model::TypeKind::StructType)) {

          model::Type *TargetFieldType = peelConstAndTypedefs(OriginalFieldType)
                                           .UnqualifiedType()
                                           .get();
          auto *OldFieldStruct = cast<model::StructType>(TargetFieldType);

          uint64_t OldFieldSize = *OldFieldStruct->size();
          rc_recur fillStructWithRecoveredDLATypeAtOffset(Model,
                                                          OldFieldStruct,
                                                          NewFieldType,
                                                          NewStart
                                                            - OriginalStart);

          revng_log(Log, "Updated field StructType: " << OldFieldStruct->ID());
          revng_assert(*OldFieldStruct->size() == OldFieldSize);
          revng_assert(OldFieldStruct->verify());
        }
      }
      ++NewFieldsIt;
    }

    for (model::StructField *NewField : CompatibleFields)
      OriginalStruct->Fields().insert(*NewField);

  } else if (auto *NewU = dyn_cast<model::UnionType>(RecoveredUnqualType)) {
    // If OriginalStruct is an not an empty struct, just leave it alone.
    if (not OriginalStruct->Fields().empty())
      rc_return;

    // If DLA recoverd an union kind, whose size is too large, we have to
    // shrink it, like we did with a struct kind.
    auto FieldsRemaining = NewU->Fields().size();
    if (RecoveredSize > OriginalStructSize) {

      const auto IsTooLarge =
        [OriginalStructSize](const model::UnionField &Field) {
          return *Field.Type().size() > OriginalStructSize;
        };

      // First, detect the fields that are too large.
      llvm::SmallSet<size_t, 8> FieldsToDrop;
      auto It = NewU->Fields().begin();
      auto End = NewU->Fields().end();
      for (; It != End; ++It) {
        revng_assert(It->verify());
        revng_assert(It->Type().verify());
        if (IsTooLarge(*It))
          FieldsToDrop.insert(It->Index());
      }

      revng_assert(not FieldsToDrop.empty());

      // Count the fields that are left in the union if we remove those
      // selected to be dropped.
      FieldsRemaining = NewU->Fields().size() - FieldsToDrop.size();

      // If we need to drop at least one field but not all of them, we have
      // to renumber the remaining fields, otherwise the union will not
      // verify.
      if (FieldsRemaining) {

        // Drop the large fields
        for (auto &FieldNum : FieldsToDrop)
          NewU->Fields().erase(FieldNum);

        // Re-enumerate the remaining fields
        for (auto &Group : llvm::enumerate(NewU->Fields()))
          Group.value().Index() = Group.index();

        revng_assert(NewU->Fields().size() == FieldsRemaining);
        revng_assert(NewU->verify());
      }
    }

    // If there are fields left in the union, then inject the union in the
    // struct as field at Offset.
    if (FieldsRemaining) {
      OriginalStruct->Fields()[Offset]
        .Type() = model::QualifiedType(Model.getTypePath(NewU), {});
    }
  } else {
    revng_abort();
  }
}

static bool updateStackFrameType(model::Function &ModelFunc,
                                 const llvm::Function &LLVMFunc,
                                 const TypeMapT &DLATypes,
                                 model::Binary &Model) {
  bool Updated = false;

  if (ModelFunc.StackFrameType().empty())
    return Updated;

  auto *OldModelStackFrameType = ModelFunc.StackFrameType().get();
  auto *OldStackFrameStruct = cast<model::StructType>(OldModelStackFrameType);
  if (not OldStackFrameStruct->Fields().empty())
    return Updated;

  bool Found = false;
  for (const auto &I : llvm::instructions(LLVMFunc)) {

    auto *Call = dyn_cast<llvm::CallInst>(&I);
    if (not Call)
      continue;

    auto *Callee = Call->getCalledFunction();
    if (not Callee or Callee->getName() != "revng_stack_frame")
      continue;

    revng_assert(not Found, "Multiple calls to revng_stack_frame");
    Found = true;

    revng_log(Log, "Updating stack for " << LLVMFunc.getName());
    LoggerIndent Indent{ Log };
    revng_log(Log, "Was " << OldStackFrameStruct->ID());

    LayoutTypePtr Key{ Call, LayoutTypePtr::fieldNumNone };

    if (auto NewTypeIt = DLATypes.find(Key); NewTypeIt != DLATypes.end()) {
      model::QualifiedType NewStack = peelConstAndTypedefs(NewTypeIt->second);

      uint64_t OldStackSize = *OldStackFrameStruct->size();
      fillStructWithRecoveredDLATypeAtOffset(Model,
                                             OldStackFrameStruct,
                                             NewStack);

      revng_log(Log,
                "Updated stack frame StructType: "
                  << ModelFunc.StackFrameType().get()->ID());
      revng_assert(isa<model::StructType>(ModelFunc.StackFrameType().get()));
      revng_assert(*ModelFunc.StackFrameType().get()->size() == OldStackSize);
      revng_assert(ModelFunc.StackFrameType().get()->verify());

      Updated = true;
    }
  }

  return Updated;
}

bool dla::updateFuncSignatures(const llvm::Module &M,
                               TupleTree<model::Binary> &Model,
                               const TypeMapT &TypeMap) {
  if (ModelLog.isEnabled())
    writeToFile(Model->toString(), "model-before-func-update.yaml");
  if (VerifyLog.isEnabled())
    revng_assert(Model->verify());

  bool Updated = false;

  for (const auto &LLVMFunc : M.functions()) {
    // Update the function's prototype
    model::Function *ModelFunc = llvmToModelFunction(*Model.get(), LLVMFunc);
    if (not ModelFunc)
      continue;

    TypePath WrappedPrototypePath = ModelFunc->prototype(*Model);
    using model::QualifiedType;
    auto PrototypePath = QualifiedType::getFunctionType(WrappedPrototypePath);
    Type *ModelPrototype = PrototypePath->get();
    revng_log(Log,
              "Updating prototype of function "
                << LLVMFunc.getNameOrAsOperand());
    Updated |= updatePrototype(*Model, ModelPrototype, &LLVMFunc, TypeMap);
    Updated |= updateStackFrameType(*ModelFunc, LLVMFunc, TypeMap, *Model);

    // Update prototypes associated to indirect calls, if any are found
    for (const auto &Inst : LLVMFunc)
      if (const auto *I = llvm::dyn_cast<llvm::CallInst>(&Inst)) {
        auto Prototype = getCallSitePrototype(*Model.get(), I);
        if (not Prototype.empty()) {
          revng_log(Log,
                    "Updating prototype of indirect call "
                      << I->getNameOrAsOperand());
          Updated |= updatePrototype(*Model, Prototype.get(), I, TypeMap);
        }
      }
  }

  if (ModelLog.isEnabled())
    writeToFile(Model->toString(), "model-after-func-update.yaml");
  if (VerifyLog.isEnabled())
    revng_assert(Model->verify());

  return Updated;
}

bool dla::updateSegmentsTypes(const llvm::Module &M,
                              TupleTree<model::Binary> &Model,
                              const TypeMapT &TypeMap) {
  bool Updated = false;

  for (const auto &F : FunctionTags::SegmentRef.functions(&M)) {
    const auto &[StartAddress, VirtualSize] = extractSegmentKeyFromMetadata(F);
    auto Segment = Model->Segments().at({ StartAddress, VirtualSize });

    // If the Segment type is missing, we have nothing to update.
    if (Segment.Type().empty())
      continue;

    // We know that the Segment's type is of StructType.
    // It's empty, we'll fill it up.
    auto *SegmentStruct = cast<model::StructType>(Segment.Type().get());
    auto SegmentStructSize = *SegmentStruct->size();

    LayoutTypePtr Key{ &F, LayoutTypePtr::fieldNumNone };
    if (auto TypeIt = TypeMap.find(Key); TypeIt != TypeMap.end()) {
      // Let's examine the recovered type by DLA.
      model::QualifiedType RecoveredSegmentType = TypeIt->second;

      fillStructWithRecoveredDLATypeAtOffset(*Model,
                                             SegmentStruct,
                                             RecoveredSegmentType);

      auto *NewSegmentType = Segment.Type().get();
      revng_log(Log, "Updated segment StructType: " << NewSegmentType->ID());
      revng_assert(isa<model::StructType>(NewSegmentType));
      revng_assert(*NewSegmentType->size() == SegmentStructSize);
      revng_assert(NewSegmentType->verify());

      Updated = true;
    }
  }

  return Updated;
}
