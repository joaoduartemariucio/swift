//===--- GenClass.cpp - Swift IR Generation For 'class' Types -----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements IR generation for class types.
//
//===----------------------------------------------------------------------===//

#include "GenClass.h"

#include "swift/ABI/Class.h"
#include "swift/AST/Attr.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/TypeMemberVisitor.h"
#include "swift/AST/Types.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"

#include "Explosion.h"
#include "GenFunc.h"
#include "GenMeta.h"
#include "GenObjC.h"
#include "GenProto.h"
#include "GenType.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "LValue.h"
#include "GenHeap.h"
#include "HeapTypeInfo.h"
#include "GenInit.h"
#include "Scope.h"
#include "Cleanup.h"


using namespace swift;
using namespace irgen;

/// Is the given class known to have a swift implementation?
static bool hasSwiftImplementation(ClassDecl *theClass) {
  // For now, assume that anything imported from Objective-C does not
  // have a swift implementation.  In the future, there may be some sort
  // of clang attribute to mark something with a swift implementation.
  return !theClass->hasClangDecl();
}

/// Does the given class have a Swift refcount?
static bool hasSwiftRefcount(ClassDecl *theClass) {
  // Scan to the root class.
  while (theClass->hasBaseClass()) {
    theClass = theClass->getBaseClass()->getClassOrBoundGenericClass();
    assert(theClass && "base type of class not a class?");
  }

  // FIXME: remove this check and update the tests.  Exporting
  // something as ObjC doesn't change how we should ref-count it.
  if (theClass->getAttrs().isObjC()) return false;

  return hasSwiftImplementation(theClass);
}

/// Emit a retain of a class pointer, using the best known retain
/// semantics for the value.
llvm::Value *IRGenFunction::emitBestRetainCall(llvm::Value *value,
                                               ClassDecl *theClass) {
  if (hasSwiftRefcount(theClass)) {
    emitRetainCall(value);
    return value;
  }
  return emitObjCRetainCall(value);
}

/// Different policies for accessing a physical field.
enum class FieldAccess : uint8_t {
  /// Instance variable offsets are constant.
  ConstantDirect,

  /// Instance variable offsets must be loaded from "direct offset"
  /// global variables.
  NonConstantDirect,

  /// Instance variable offsets are kept in fields in metadata, but
  /// the offsets of those fields within the metadata are constant.
  ConstantIndirect,

  /// Instance variable offsets are kept in fields in metadata, and
  /// the offsets of those fields within the metadata must be loaded
  /// from "indirect offset" global variables.
  NonConstantIndirect
};

namespace {
  class FieldEntry {
    llvm::PointerIntPair<VarDecl*, 2> VarAndAccess;
  public:
    FieldEntry(VarDecl *var, FieldAccess access)
      : VarAndAccess(var, unsigned(access)) {}

    VarDecl *getVar() const {
      return VarAndAccess.getPointer();
    }
    FieldAccess getAccess() const {
      return FieldAccess(VarAndAccess.getInt());
    }
  };

  /// Layout information for class types.
  class ClassTypeInfo : public HeapTypeInfo<ClassTypeInfo> {
    ClassDecl *TheClass;
    mutable HeapLayout *Layout;

    /// Can we use swift reference-counting, or do we have to use
    /// objc_retain/release?
    bool HasSwiftRefcount;

  public:
    ClassTypeInfo(llvm::PointerType *irType, Size size, Alignment align,
                  ClassDecl *D)
      : HeapTypeInfo(irType, size, align), TheClass(D), Layout(nullptr) {

      HasSwiftRefcount = ::hasSwiftRefcount(D);
    }

    bool hasSwiftRefcount() const {
      return HasSwiftRefcount;
    }

    ~ClassTypeInfo() {
      delete Layout;
    }

    ClassDecl *getClass() const { return TheClass; }

    static void addFieldsFromClass(IRGenModule &IGM,
                                   ClassDecl *theClass,
                              SmallVectorImpl<const TypeInfo*> &fieldTypes) {
      // Recursively add members from the base.  This really only
      // works under an assumption that the class is a base
      if (theClass->hasBaseClass()) {
        auto baseClass =
          theClass->getBaseClass()->getClassOrBoundGenericClass();
        addFieldsFromClass(IGM, baseClass, fieldTypes);
      }
      for (Decl *member : theClass->getMembers())
        if (VarDecl *var = dyn_cast<VarDecl>(member))
          if (!var->isProperty())
            fieldTypes.push_back(&IGM.getFragileTypeInfo(var->getType()));
    }

    const HeapLayout &getLayout(IRGenModule &IGM) const {
      if (Layout)
        return *Layout;

      // Collect all the fields from the type.
      SmallVector<const TypeInfo*, 8> fieldTypes;
      addFieldsFromClass(IGM, getClass(), fieldTypes);

      llvm::PointerType *Ptr = cast<llvm::PointerType>(getStorageType());
      llvm::StructType *STy = cast<llvm::StructType>(Ptr->getElementType());

      Layout = new HeapLayout(IGM, LayoutStrategy::Optimal, fieldTypes, STy);
      return *Layout;
    }
    Alignment getHeapAlignment(IRGenModule &IGM) const {
      return getLayout(IGM).getAlignment();
    }
    llvm::ArrayRef<ElementLayout> getElements(IRGenModule &IGM) const {
      return getLayout(IGM).getElements();
    }
  };

  /// A class for computing properties of the instance-variable layout
  /// of a class.  TODO: cache the results!
  class LayoutClass {
    IRGenModule &IGM;

    ClassDecl *Root;
    SmallVector<FieldEntry, 8> Fields;

    bool IsMetadataResilient = false;
    bool IsObjectResilient = false;
    bool IsObjectGenericallyArranged = false;

    ResilienceScope Resilience;

  public:
    LayoutClass(IRGenModule &IGM, ResilienceScope resilience,
                ClassDecl *theClass, CanType type)
        : IGM(IGM), Resilience(resilience) {
      layout(theClass, type);
    }

    /// The root class for purposes of metaclass objects.
    ClassDecl *getRootClassForMetaclass() const {
      // If the formal root class is imported from Objective-C, then
      // we should use that.  For a class that's really implemented in
      // Objective-C, this is obviously right.  For a class that's
      // really implemented in Swift, but that we're importing via an
      // Objective-C interface, this would be wrong --- except such a
      // class can never be a formal root class, because a Swift class
      // without a formal superclass will actually be parented by
      // SwiftObject (or maybe eventually something else like it),
      // which will be visible in the Objective-C type system.
      if (Root->hasClangDecl()) return Root;

      return IGM.getSwiftRootClass();
    }

    const FieldEntry &getFieldEntry(VarDecl *field) const {
      for (auto &entry : Fields)
        if (entry.getVar() == field)
          return entry;
      llvm_unreachable("no entry for field!");
    }

  private:
    void layout(ClassDecl *theClass, CanType type) {
      // TODO: use the full type information to potentially make
      // generic layouts concrete.

      // First, collect information about the base class.
      if (theClass->hasBaseClass()) {
        CanType baseType = theClass->getBaseClass()->getCanonicalType();
        auto baseClass = type->getClassOrBoundGenericClass();
        assert(baseClass);
        layout(baseClass, baseType);
      } else {
        Root = theClass;
      }

      // If the class is resilient, then it may have fields we can't
      // see, and all subsequent fields are *at least* resilient ---
      // and if the class is generic, then it may have
      // dependently-sized fields, and we'll be in the worst case.
      bool isClassResilient = IGM.isResilient(theClass, Resilience);
      if (isClassResilient) {
        IsMetadataResilient = true;
        IsObjectResilient = true;
        if (theClass->getGenericParamsOfContext() != nullptr) {
          IsObjectGenericallyArranged = true;
        }
      }

      // Okay, make entries for all the physical fields we know about.
      for (auto member : theClass->getMembers()) {
        auto var = dyn_cast<VarDecl>(member);
        if (!var) continue;

        // Skip properties that we have to access logically.
        assert(isClassResilient || !IGM.isResilient(var, Resilience));
        if (var->isProperty())
          continue;

        Fields.push_back(FieldEntry(var, getCurFieldAccess()));

        // Adjust based on the type of this field.
        // FIXME: this algorithm is assuming that fields are laid out
        // in declaration order.
        adjustAccessAfterField(var);
      }
    }

    FieldAccess getCurFieldAccess() const {
      if (IsObjectGenericallyArranged) {
        if (IsMetadataResilient) {
          return FieldAccess::NonConstantIndirect;
        } else {
          return FieldAccess::ConstantIndirect;
        }
      } else {
        if (IsObjectResilient) {
          return FieldAccess::NonConstantDirect;
        } else {
          return FieldAccess::ConstantDirect;
        }
      }
    }

    void adjustAccessAfterField(VarDecl *var) {
      if (var->isProperty()) return;

      CanType type = var->getType()->getCanonicalType();
      switch (IGM.classifyTypeSize(type, ResilienceScope::Local)) {
      case ObjectSize::Fixed:
        return;
      case ObjectSize::Resilient:
        IsObjectResilient = true;
        return;
      case ObjectSize::Dependent:
        IsObjectResilient = IsObjectGenericallyArranged = true;
        return;
      }
      llvm_unreachable("bad ObjectSize value");
    }
  };
}  // end anonymous namespace.

static unsigned getNumFieldsInBases(ClassDecl *derived, unsigned count = 0) {
  if (!derived->hasBaseClass()) return count;
  auto base = derived->getBaseClass()->getClassOrBoundGenericClass();
  for (Decl *member : base->getMembers()) {
    if (auto var = dyn_cast<VarDecl>(member))
      if (!var->isProperty())
        ++count;
  }
  return getNumFieldsInBases(base, count);
}

static unsigned getFieldIndex(ClassDecl *base, VarDecl *target) {
  // FIXME: This is an ugly hack.
  unsigned index = getNumFieldsInBases(base);
  for (Decl *member : base->getMembers()) {
    if (member == target) return index;
    if (auto var = dyn_cast<VarDecl>(member))
      if (!var->isProperty())
        ++index;
  }
  llvm_unreachable("didn't find field in type!");
}

/// Cast the base to i8*, apply the given inbounds offset, and cast to
/// a pointer to the given type.
static llvm::Value *emitGEPToOffset(IRGenFunction &IGF,
                                    llvm::Value *base,
                                    llvm::Value *offset,
                                    llvm::Type *type,
                                    const llvm::Twine &name = "") {
  auto addr = IGF.Builder.CreateBitCast(base, IGF.IGM.Int8PtrTy);
  addr = IGF.Builder.CreateInBoundsGEP(addr, offset);
  return IGF.Builder.CreateBitCast(addr, type->getPointerTo(), name);
}

/// Emit a field l-value by applying the given offset to the given base.
static LValue emitLValueAtOffset(IRGenFunction &IGF,
                                 llvm::Value *base, llvm::Value *offset,
                                 VarDecl *field) {
  auto &fieldTI = IGF.getFragileTypeInfo(field->getType());
  auto addr = emitGEPToOffset(IGF, base, offset, fieldTI.getStorageType(),
                              base->getName() + "." + field->getName().str());
  Address fieldAddr(addr, fieldTI.StorageAlignment);
  return IGF.emitAddressLValue(OwnedAddress(fieldAddr, base));
}

static LValue emitPhysicalClassMemberLValue(IRGenFunction &IGF,
                                            Expr *baseE,
                                            ClassDecl *baseClass,
                                            const ClassTypeInfo &baseClassTI,
                                            VarDecl *field) {
  Explosion explosion(ExplosionKind::Maximal);
  // FIXME: Can we avoid the retain/release here in some cases?
  IGF.emitRValue(baseE, explosion);
  ManagedValue baseVal = explosion.claimNext();
  llvm::Value *base = baseVal.getValue();

  auto baseType = baseE->getType()->getCanonicalType();
  LayoutClass layout(IGF.IGM, ResilienceScope::Local, baseClass, baseType);
  
  auto &entry = layout.getFieldEntry(field);
  switch (entry.getAccess()) {
  case FieldAccess::ConstantDirect: {
    // FIXME: This field index computation is an ugly hack.
    unsigned fieldIndex = getFieldIndex(baseClass, field);

    Address baseAddr(base, baseClassTI.getHeapAlignment(IGF.IGM));
    auto &element = baseClassTI.getElements(IGF.IGM)[fieldIndex];
    Address memberAddr = element.project(IGF, baseAddr);
    return IGF.emitAddressLValue(OwnedAddress(memberAddr, base));
  }

  case FieldAccess::NonConstantDirect: {
    Address offsetA = IGF.IGM.getAddrOfFieldOffset(field, /*indirect*/ false);
    auto offset = IGF.Builder.CreateLoad(offsetA, "offset");
    return emitLValueAtOffset(IGF, base, offset, field);
  }

  case FieldAccess::ConstantIndirect: {
    auto metadata = emitHeapMetadataRefForHeapObject(IGF, base, baseType);
    auto offset = emitClassFieldOffset(IGF, baseClass, field, metadata);
    return emitLValueAtOffset(IGF, base, offset, field);
  }

  case FieldAccess::NonConstantIndirect: {
    auto metadata = emitHeapMetadataRefForHeapObject(IGF, base, baseType);
    Address indirectOffsetA =
      IGF.IGM.getAddrOfFieldOffset(field, /*indirect*/ true);
    auto indirectOffset =
      IGF.Builder.CreateLoad(indirectOffsetA, "indirect-offset");
    auto offsetA =
      emitGEPToOffset(IGF, metadata, indirectOffset, IGF.IGM.SizeTy);
    auto offset =
      IGF.Builder.CreateLoad(Address(offsetA, IGF.IGM.getPointerAlignment()));
    return emitLValueAtOffset(IGF, base, offset, field);
  }
  }
  llvm_unreachable("bad field-access strategy");
}

LValue irgen::emitPhysicalClassMemberLValue(IRGenFunction &IGF,
                                            MemberRefExpr *E) {
  auto baseType = E->getBase()->getType()->castTo<ClassType>();
  auto &baseTI = IGF.getFragileTypeInfo(baseType).as<ClassTypeInfo>();
  return ::emitPhysicalClassMemberLValue(IGF, E->getBase(),
                                         baseType->getDecl(), baseTI,
                                         E->getDecl());
}

LValue irgen::emitPhysicalClassMemberLValue(IRGenFunction &IGF,
                                            GenericMemberRefExpr *E) {
  auto baseType = E->getBase()->getType()->castTo<BoundGenericClassType>();
  auto &baseTI = IGF.getFragileTypeInfo(baseType).as<ClassTypeInfo>();
  return ::emitPhysicalClassMemberLValue(IGF, E->getBase(), baseType->getDecl(),
                                         baseTI, cast<VarDecl>(E->getDecl()));
}

namespace {
  class ClassDestroyCleanup : public Cleanup {
    llvm::Value *ThisValue;
    const ClassTypeInfo &info;

  public:
    ClassDestroyCleanup(llvm::Value *ThisValue, const ClassTypeInfo &info)
      : ThisValue(ThisValue), info(info) {}

    void emit(IRGenFunction &IGF) const {
      // FIXME: This implementation will be wrong once we get dynamic
      // class layout.
      auto &layout = info.getLayout(IGF.IGM);
      Address baseAddr = layout.emitCastOfAlloc(IGF, ThisValue);

      // Destroy all the instance variables of the class.
      for (auto &field : layout.getElements()) {
        if (field.Type->isPOD(ResilienceScope::Local))
          continue;
        
        field.Type->destroy(IGF, field.project(IGF, baseAddr));
      }
    }
  };
}

/// Emit the destructor for a class.
///
/// \param DD - the optional explicit destructor declaration
static void emitClassDestructor(IRGenModule &IGM, ClassDecl *CD,
                                DestructorDecl *DD) {
  llvm::Function *fn = IGM.getAddrOfDestructor(CD);

  IRGenFunction IGF(IGM, CanType(), nullptr,
                    ExplosionKind::Minimal, 0, fn, Prologue::Bare);

  Type thisType = CD->getDeclaredTypeInContext();
  const ClassTypeInfo &info =
      IGM.getFragileTypeInfo(thisType).as<ClassTypeInfo>();
  llvm::Value *thisValue = fn->arg_begin();
  thisValue = IGF.Builder.CreateBitCast(thisValue, info.getStorageType());

  // Bind generic parameters.  This is only really necessary if we
  // have either (1) an explicit destructor or (2) something dependent
  // to destroy implicitly.
  assert((!DD || DD->getDeclContext() == CD) &&
         "destructor not defined in main class decl; archetypes might be off");
  if (auto generics = CD->getGenericParamsOfContext()) {
    Explosion fakeArgs(ExplosionKind::Minimal);
    fakeArgs.addUnmanaged(thisValue);
    fakeArgs.claimUnmanagedNext();

    auto argType = CD->getDeclaredTypeInContext()->getCanonicalType();
    auto polyFn =
      PolymorphicFunctionType::get(argType,
                                   TupleType::getEmpty(IGF.IGM.Context),
                                   generics,
                                   IGF.IGM.Context);
    emitPolymorphicParameters(IGF, polyFn, fakeArgs);
  }

  // FIXME: If the class is generic, we need some way to get at the
  // witness table.

  // FIXME: This extra retain call is sort of strange, but it's necessary
  // for the moment to prevent re-triggering destruction.
  IGF.emitRetainCall(thisValue);

  Scope scope(IGF);
  IGF.pushCleanup<ClassDestroyCleanup>(thisValue, info);

  if (DD) {
    auto thisDecl = DD->getImplicitThisDecl();
    Initialization I;
    I.registerObject(IGF, I.getObjectForDecl(thisDecl),
                      thisDecl->hasFixedLifetime() ? NotOnHeap : OnHeap, info);
    Address addr = I.emitVariable(IGF, thisDecl, info);
    Explosion thisE(ExplosionKind::Maximal);
    IGF.emitRetain(thisValue, thisE);
    info.initialize(IGF, thisE, addr);
    I.markInitialized(IGF, I.getObjectForDecl(thisDecl));

    IGF.emitFunctionTopLevel(DD->getBody());
  }
  scope.pop();

  if (IGF.Builder.hasValidIP()) {
    llvm::Value *size = info.getLayout(IGM).emitSize(IGF);
    IGF.Builder.CreateRet(size);
  }
}

static void emitClassConstructor(IRGenModule &IGM, ConstructorDecl *CD) {
  llvm::Function *fn = IGM.getAddrOfConstructor(CD, ExplosionKind::Minimal);
  auto thisDecl = CD->getImplicitThisDecl();
  CanType thisType = thisDecl->getType()->getCanonicalType();
  auto &classTI = IGM.getFragileTypeInfo(thisType).as<ClassTypeInfo>();
  auto &layout = classTI.getLayout(IGM);

  Pattern* pats[] = {
    new (IGM.Context) AnyPattern(SourceLoc()),
    CD->getArguments()
  };
  pats[0]->setType(MetaTypeType::get(thisDecl->getType(), IGM.Context));
  IRGenFunction IGF(IGM, CD->getType()->getCanonicalType(), pats,
                    ExplosionKind::Minimal, 1, fn, Prologue::Standard);

  // Emit the "this" variable
  Initialization I;
  auto object = I.getObjectForDecl(thisDecl);
  I.registerObject(IGF, object,
                   thisDecl->hasFixedLifetime() ? NotOnHeap : OnHeap,
                   classTI);
  Address addr = I.emitVariable(IGF, thisDecl, classTI);

  if (!CD->getAllocThisExpr()) {
    FullExpr scope(IGF);
    // Allocate the class.
    // FIXME: Long-term, we clearly need a specialized runtime entry point.

    llvm::Value *metadata = emitClassHeapMetadataRef(IGF, thisType);

    llvm::Value *size = layout.emitSize(IGF);
    llvm::Value *align = layout.emitAlign(IGF);
    llvm::Value *val = IGF.emitAllocObjectCall(metadata, size, align,
                                               "reference.new");
    llvm::Type *destType = layout.getType()->getPointerTo();
    llvm::Value *castVal = IGF.Builder.CreateBitCast(val, destType);
    IGF.Builder.CreateStore(castVal, addr);

    scope.pop();

    I.markInitialized(IGF, I.getObjectForDecl(thisDecl));
  } else {
    // Use the allocation expression described in the AST to initialize 'this'.
    I.emitInit(IGF, object, addr, CD->getAllocThisExpr(), classTI);
  }
  
  IGF.emitConstructorBody(CD);
}

void IRGenModule::emitClassConstructor(ConstructorDecl *D) {
  return ::emitClassConstructor(*this, D);
}

/// emitClassDecl - Emit all the declarations associated with this class type.
void IRGenModule::emitClassDecl(ClassDecl *D) {
  PrettyStackTraceDecl prettyStackTrace("emitting class metadata for", D);

  auto &classTI = Types.getFragileTypeInfo(D).as<ClassTypeInfo>();
  auto &layout = classTI.getLayout(*this);

  // Emit the class metadata.  [objc] on a class is basically an
  // 'extern' declaration and suppresses this.
  if (!D->getAttrs().isObjC())
    emitClassMetadata(*this, D, layout);

  bool emittedDtor = false;

  // FIXME: This is mostly copy-paste from emitExtension;
  // figure out how to refactor! 
  for (Decl *member : D->getMembers()) {
    switch (member->getKind()) {
    case DeclKind::Import:
    case DeclKind::TopLevelCode:
    case DeclKind::Protocol:
    case DeclKind::OneOfElement:
    case DeclKind::Extension:
      llvm_unreachable("decl not allowed in class!");

    // We can have meaningful initializers for variables, but
    // we can't handle them yet.  For the moment, just ignore them.
    case DeclKind::PatternBinding:
      continue;

    case DeclKind::Subscript:
      // Getter/setter will be handled separately.
      continue;

    case DeclKind::TypeAlias:
      continue;
    case DeclKind::OneOf:
      emitOneOfDecl(cast<OneOfDecl>(member));
      continue;
    case DeclKind::Struct:
      emitStructDecl(cast<StructDecl>(member));
      continue;
    case DeclKind::Class:
      emitClassDecl(cast<ClassDecl>(member));
      continue;
    case DeclKind::Var:
      if (cast<VarDecl>(member)->isProperty())
        // Getter/setter will be handled separately.
        continue;
      // FIXME: Will need an implementation here for resilience
      continue;
    case DeclKind::Func: {
      FuncDecl *func = cast<FuncDecl>(member);
      if (func->isStatic()) {
        // Eventually this won't always be the right thing.
        emitStaticMethod(func);
      } else {
        emitInstanceMethod(func);
      }
      continue;
    }
    case DeclKind::Constructor: {
      ::emitClassConstructor(*this, cast<ConstructorDecl>(member));
      continue;
    }
    case DeclKind::Destructor: {
      assert(!emittedDtor && "two destructors in class?");
      emittedDtor = true;
      emitClassDestructor(*this, D, cast<DestructorDecl>(member));
      continue;
    }
    }
    llvm_unreachable("bad extension member kind");
  }

  // Emit a defaulted class destructor if we didn't see one explicitly.
  if (!emittedDtor)
    emitClassDestructor(*this, D, nullptr);
}

namespace {
  enum ForMetaClass_t : bool {
    ForClass = false,
    ForMetaClass = true
  };

  /// A class for building class data.
  ///
  /// In Objective-C terms, this is the class_ro_t.
  class ClassDataBuilder : public ClassMemberVisitor<ClassDataBuilder> {
    IRGenModule &IGM;
    ClassDecl *TheClass;
    const LayoutClass &Layout;

    bool HasNonTrivialDestructor = false;
    bool HasNonTrivialConstructor = false;
    SmallVector<llvm::Constant*, 8> Ivars;
    SmallVector<llvm::Constant*, 16> InstanceMethods;
    SmallVector<llvm::Constant*, 16> ClassMethods;
    SmallVector<llvm::Constant*, 4> Protocols;
    SmallVector<llvm::Constant*, 8> Properties;
    llvm::Constant *Name = nullptr;
  public:
    ClassDataBuilder(IRGenModule &IGM, ClassDecl *theClass,
                     const LayoutClass &layout)
        : IGM(IGM), TheClass(theClass), Layout(layout) {
      visitMembers(TheClass);
    }

    /// Build the metaclass stub object.
    void buildMetaclassStub() {
      // The isa is the metaclass pointer for the root class.
      auto rootClass = Layout.getRootClassForMetaclass();
      auto rootPtr = IGM.getAddrOfMetaclassObject(rootClass);

      // The superclass of the metaclass is the metaclass of the
      // superclass.  Note that for metaclass stubs, we can always
      // ignore parent contexts and generic arguments.
      //
      // If this class has no formal superclass, then its actual
      // superclass is SwiftObject, i.e. the root class.
      llvm::Constant *superPtr;
      if (TheClass->hasBaseClass()) {
        auto base = TheClass->getBaseClass()->getClassOrBoundGenericClass();
        superPtr = IGM.getAddrOfMetaclassObject(base);
      } else {
        superPtr = rootPtr;
      }

      auto dataPtr = emitROData(ForMetaClass);
      dataPtr = llvm::ConstantExpr::getPtrToInt(dataPtr, IGM.IntPtrTy);

      llvm::Constant *fields[] = {
        rootPtr, superPtr, null(), null(), dataPtr
      };
      auto init = llvm::ConstantStruct::get(IGM.ObjCClassStructTy,
                                            makeArrayRef(fields));
      auto metaclass =
        cast<llvm::GlobalVariable>(IGM.getAddrOfMetaclassObject(TheClass));
      metaclass->setInitializer(init);
    }

    llvm::Constant *emitROData(ForMetaClass_t forMeta) {
      SmallVector<llvm::Constant*, 11> fields;
      // struct _class_ro_t {
      //   uint32_t flags;
      fields.push_back(buildFlags(forMeta));

      //   uint32_t instanceStart;
      //   uint32_t instanceSize;
      // These are generally filled in by the runtime.
      // TODO: it's an optimization to have them have the right values
      // at launch-time.
      auto zero32 = llvm::ConstantInt::get(IGM.Int32Ty, 0);
      fields.push_back(zero32);
      fields.push_back(zero32);

      //   uint32_t reserved;  // only when building for 64bit targets
      if (IGM.getPointerAlignment().getValue() > 4) {
        assert(IGM.getPointerAlignment().getValue() == 8);
        fields.push_back(zero32);
      }

      //   const uint8_t *ivarLayout;
      // GC/ARC layout.  TODO.
      fields.push_back(null());

      //   const char *name;
      // It is correct to use the same name for both class and metaclass.
      fields.push_back(buildName());

      //   const method_list_t *baseMethods;
      fields.push_back(forMeta ? buildClassMethodList()
                               : buildInstanceMethodList());

      //   const protocol_list_t *baseProtocols;
      // Apparently, this list is the same in the class and the metaclass.
      fields.push_back(buildProtocolList());

      //   const ivar_list_t *ivars;
      fields.push_back(forMeta ? null() : buildIvarList());

      //   const uint8_t *weakIvarLayout;
      // More GC/ARC layout.  TODO.
      fields.push_back(null());

      //   const property_list_t *baseProperties;
      fields.push_back(forMeta ? null() : buildPropertyList());

      // };

      auto dataSuffix = forMeta ? "_METACLASS_DATA_" : "_DATA_";
      return buildGlobalVariable(fields, dataSuffix);
    }

  private:
    llvm::Constant *buildFlags(ForMetaClass_t forMeta) {
      ClassFlags flags = ClassFlags::CompiledByARC;

      // Mark metaclasses as appropriate.
      if (forMeta) {
        flags |= ClassFlags::Meta;

      // Non-metaclasses need us to record things whether primitive
      // construction/destructor is trivial.
      } else if (HasNonTrivialDestructor || HasNonTrivialConstructor) {
        flags |= ClassFlags::HasCXXStructors;
        if (!HasNonTrivialConstructor)
          flags |= ClassFlags::HasCXXDestructorOnly;
      }

      // FIXME: set ClassFlags::Hidden when appropriate
      return llvm::ConstantInt::get(IGM.Int32Ty, uint32_t(flags));
    }

    llvm::Constant *buildName() {
      if (Name) return Name;
      return (Name = IGM.getAddrOfGlobalString(TheClass->getName().str()));
    }

    llvm::Constant *null() {
      return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
    }

    /*** Methods ***********************************************************/

  public:
    /// Methods need to be collected into the appropriate methods list.
    void visitFuncDecl(FuncDecl *method) {
      if (!requiresObjCMethodDescriptor(method)) return;
      llvm::Constant *entry = emitObjCMethodDescriptor(IGM, method);
      if (!method->isStatic()) {
        InstanceMethods.push_back(entry);
      } else {
        ClassMethods.push_back(entry);
      }
    }

  private:
    bool requiresObjCMethodDescriptor(FuncDecl *method) {
      if (method->getAttrs().ObjC) return true;
      if (auto override = method->getOverriddenDecl())
        return requiresObjCMethodDescriptor(override);
      return false;
    }

    llvm::Constant *buildClassMethodList()  {
      return buildMethodList(ClassMethods, "_CLASS_METHODS_");
    }

    llvm::Constant *buildInstanceMethodList()  {
      return buildMethodList(InstanceMethods, "_INSTANCE_METHODS_");
    }

    /// struct method_list_t {
    ///   uint32_t entsize; // runtime uses low bits for its own purposes
    ///   uint32_t count;
    ///   method_t list[count];
    /// };
    ///
    /// This method does not return a value of a predictable type.
    llvm::Constant *buildMethodList(ArrayRef<llvm::Constant*> methods,
                                    StringRef name) {
      return buildOptionalList(methods, 3 * IGM.getPointerSize(), name);
    }

    /*** Protocols *********************************************************/

    /// typedef uintptr_t protocol_ref_t;  // protocol_t*, but unremapped
    llvm::Constant *buildProtocolRef(ProtocolDecl *protocol) {
      // FIXME
      return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
    }
    
    /// struct protocol_list_t {
    ///   uintptr_t count;
    ///   protocol_ref_t[count];
    /// };
    ///
    /// This method does not return a value of a predictable type.
    llvm::Constant *buildProtocolList() {
      return buildOptionalList(Protocols, Size(0), "_PROTOCOLS_");
    }

    /*** Ivars *************************************************************/

  public:
    /// Variables might be properties or ivars.
    void visitVarDecl(VarDecl *var) {
      if (var->isProperty()) {
        visitProperty(var);
      } else {
        visitIvar(var);
      }
    }

  private:
    /// Ivars need to be collected in the ivars list, and they also
    /// affect flags.
    void visitIvar(VarDecl *var) {
      Ivars.push_back(buildIvar(var));
      if (!IGM.isPOD(var->getType()->getCanonicalType(),
                     ResilienceScope::Local)) {
        HasNonTrivialDestructor = true;
      }
    }

    /// struct ivar_t {
    ///   uintptr_t *offset;
    ///   const char *name;
    ///   const char *type;
    ///   uint32_t alignment;
    ///   uint32_t size;
    /// };
    llvm::Constant *buildIvar(VarDecl *ivar) {
      // FIXME: this is not always the right thing to do!
      auto offsetAddr = IGM.getAddrOfFieldOffset(ivar, /*direct*/ true);
      auto offsetVar = cast<llvm::GlobalVariable>(offsetAddr.getAddress());
      offsetVar->setConstant(false);
      offsetVar->setInitializer(llvm::ConstantInt::get(IGM.IntPtrTy, 0));

      // TODO: clang puts this in __TEXT,__objc_methname,cstring_literals
      auto name = IGM.getAddrOfGlobalString(ivar->getName().str());

      // TODO: clang puts this in __TEXT,__objc_methtype,cstring_literals
      auto typeEncode = llvm::ConstantPointerNull::get(IGM.Int8PtrTy);

      auto &ivarTI = IGM.getFragileTypeInfo(ivar->getType());
      auto size = ivarTI.getStaticSize(IGM);
      auto alignment = ivarTI.getStaticAlignment(IGM);
      assert((size != nullptr) == (alignment != nullptr));
      if (size != nullptr) {
        if (IGM.SizeTy != IGM.Int32Ty) {
          size = llvm::ConstantExpr::getTrunc(size, IGM.Int32Ty);
          alignment = llvm::ConstantExpr::getTrunc(alignment, IGM.Int32Ty);
        }
      } else {
        size = alignment = llvm::ConstantInt::get(IGM.Int32Ty, 0);
      }

      llvm::Constant *fields[] = {
        offsetVar,
        name,
        typeEncode,
        size,
        alignment
      };
      return llvm::ConstantStruct::getAnon(IGM.getLLVMContext(), fields);
    }

    /// struct ivar_list_t {
    ///   uint32_t entsize;
    ///   uint32_t count;
    ///   ivar_t list[count];
    /// };
    ///
    /// This method does not return a value of a predictable type.
    llvm::Constant *buildIvarList() {
      Size eltSize = 3 * IGM.getPointerSize() + Size(8);
      return buildOptionalList(Ivars, eltSize, "_IVARS_");
    }

    /*** Properties ********************************************************/

    /// Properties need to be collected in the properties list.
    void visitProperty(VarDecl *var) {
      Properties.push_back(buildProperty(var));
    }

    /// struct property_t {
    ///   const char *name;
    ///   const char *attributes;
    /// };
    llvm::Constant *buildProperty(VarDecl *prop) {
      // FIXME
      return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
    }

    /// struct property_list_t {
    ///   uint32_t entsize;
    ///   uint32_t count;
    ///   property_t list[count];
    /// };
    ///
    /// This method does not return a value of a predictable type.
    llvm::Constant *buildPropertyList() {
      Size eltSize = 2 * IGM.getPointerSize();
      return buildOptionalList(Properties, eltSize, "_PROPERTIES_");
    }

    /*** General ***********************************************************/

    /// Build a list structure from the given array of objects.
    /// If the array is empty, use null.  The assumption is that every
    /// initializer has the same size.
    ///
    /// \param optionalEltSize - if non-zero, a size which needs
    ///   to be placed in the list header
    llvm::Constant *buildOptionalList(ArrayRef<llvm::Constant*> objects,
                                      Size optionalEltSize,
                                      StringRef nameBase) {
      if (objects.empty())
        return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);

      SmallVector<llvm::Constant*, 3> fields;

      // In all of the foo_list_t structs, either:
      //   - there's a 32-bit entry size and a 32-bit count or
      //   - there's no entry size and a uintptr_t count.
      if (!optionalEltSize.isZero()) {
        fields.push_back(llvm::ConstantInt::get(IGM.Int32Ty,
                                                optionalEltSize.getValue()));
        fields.push_back(llvm::ConstantInt::get(IGM.Int32Ty, objects.size()));
      } else {
        fields.push_back(llvm::ConstantInt::get(IGM.IntPtrTy, objects.size()));
      }

      auto arrayTy =
        llvm::ArrayType::get(objects[0]->getType(), objects.size());
      fields.push_back(llvm::ConstantArray::get(arrayTy, objects));

      return buildGlobalVariable(fields, nameBase);
    }

    /// Build a private global variable as a structure containing the
    /// given fields.
    llvm::Constant *buildGlobalVariable(ArrayRef<llvm::Constant*> fields,
                                        StringRef nameBase) {
      auto init = llvm::ConstantStruct::getAnon(IGM.getLLVMContext(), fields);
      auto var = new llvm::GlobalVariable(IGM.Module, init->getType(),
                                          /*constant*/ true,
                                          llvm::GlobalVariable::PrivateLinkage,
                                          init,
                                          Twine(nameBase) 
                                            + TheClass->getName().str());
      var->setAlignment(IGM.getPointerAlignment().getValue());
      var->setSection("__DATA, __objc_const");
      return var;
    }

  public:
    /// Member types don't get any representation.
    /// Maybe this should change for reflection purposes?
    void visitTypeDecl(TypeDecl *type) {}

    /// Pattern-bindings don't require anything special as long as
    /// these initializations are performed in the constructor, not
    /// .cxx_construct.
    void visitPatternBindingDecl(PatternBindingDecl *binding) {}

    /// Subscripts should probably be collected in extended metadata.
    void visitSubscriptDecl(SubscriptDecl *subscript) {
      // TODO
    }

    /// Constructors should probably be collected in extended metadata.
    void visitConstructorDecl(ConstructorDecl *ctor) {
      // TODO
    }

    /// The destructor doesn't really require any special
    /// representation here.
    void visitDestructorDecl(DestructorDecl *dtor) {}
  };
}

/// Emit the private data (RO-data) associated with a class.
llvm::Constant *irgen::emitClassPrivateData(IRGenModule &IGM,
                                            ClassDecl *cls) {
  CanType type = cls->getDeclaredTypeInContext()->getCanonicalType();
  LayoutClass layout(IGM, ResilienceScope::Universal, cls, type);
  ClassDataBuilder builder(IGM, cls, layout);

  // First, build the metaclass object.
  builder.buildMetaclassStub();

  // Then build the class RO-data.
  return builder.emitROData(ForClass);
}

const TypeInfo *TypeConverter::convertClassType(ClassDecl *D) {
  llvm::StructType *ST = IGM.createNominalType(D);
  llvm::PointerType *irType = ST->getPointerTo();
  return new ClassTypeInfo(irType, IGM.getPointerSize(),
                           IGM.getPointerAlignment(), D);
}

/// Lazily declare the Swift root-class, SwiftObject.
ClassDecl *IRGenModule::getSwiftRootClass() {
  if (SwiftRootClass) return SwiftRootClass;

  auto name = Context.getIdentifier("SwiftObject");

  // Make a really fake-looking class.
  SwiftRootClass = new (Context) ClassDecl(SourceLoc(), name, SourceLoc(),
                                           MutableArrayRef<TypeLoc>(),
                                           /*generics*/ nullptr,
                                           Context.TheBuiltinModule);
  SwiftRootClass->getMutableAttrs().ObjC = true;
  return SwiftRootClass;
}
