//===-- Constants.cpp - Implement Constant nodes --------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the Constant* classes...
//
//===----------------------------------------------------------------------===//

#include "llvm/Constants.h"
#include "ConstantFold.h"
#include "llvm/DerivedTypes.h"
#include "llvm/GlobalValue.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>
#include <map>
using namespace llvm;

//===----------------------------------------------------------------------===//
//                              Constant Class
//===----------------------------------------------------------------------===//

void Constant::destroyConstantImpl() {
  // When a Constant is destroyed, there may be lingering
  // references to the constant by other constants in the constant pool.  These
  // constants are implicitly dependent on the module that is being deleted,
  // but they don't know that.  Because we only find out when the CPV is
  // deleted, we must now notify all of our users (that should only be
  // Constants) that they are, in fact, invalid now and should be deleted.
  //
  while (!use_empty()) {
    Value *V = use_back();
#ifndef NDEBUG      // Only in -g mode...
    if (!isa<Constant>(V))
      DOUT << "While deleting: " << *this
           << "\n\nUse still stuck around after Def is destroyed: "
           << *V << "\n\n";
#endif
    assert(isa<Constant>(V) && "References remain to Constant being destroyed");
    Constant *CV = cast<Constant>(V);
    CV->destroyConstant();

    // The constant should remove itself from our use list...
    assert((use_empty() || use_back() != V) && "Constant not removed!");
  }

  // Value has no outstanding references it is safe to delete it now...
  delete this;
}

/// canTrap - Return true if evaluation of this constant could trap.  This is
/// true for things like constant expressions that could divide by zero.
bool Constant::canTrap() const {
  assert(getType()->isFirstClassType() && "Cannot evaluate aggregate vals!");
  // The only thing that could possibly trap are constant exprs.
  const ConstantExpr *CE = dyn_cast<ConstantExpr>(this);
  if (!CE) return false;
  
  // ConstantExpr traps if any operands can trap. 
  for (unsigned i = 0, e = getNumOperands(); i != e; ++i)
    if (getOperand(i)->canTrap()) 
      return true;

  // Otherwise, only specific operations can trap.
  switch (CE->getOpcode()) {
  default:
    return false;
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
  case Instruction::URem:
  case Instruction::SRem:
  case Instruction::FRem:
    // Div and rem can trap if the RHS is not known to be non-zero.
    if (!isa<ConstantInt>(getOperand(1)) || getOperand(1)->isNullValue())
      return true;
    return false;
  }
}

/// ContaintsRelocations - Return true if the constant value contains
/// relocations which cannot be resolved at compile time.
bool Constant::ContainsRelocations() const {
  if (isa<GlobalValue>(this))
    return true;
  for (unsigned i = 0, e = getNumOperands(); i != e; ++i)
    if (getOperand(i)->ContainsRelocations())
      return true;
  return false;
}

// Static constructor to create a '0' constant of arbitrary type...
Constant *Constant::getNullValue(const Type *Ty) {
  static uint64_t zero[2] = {0, 0};
  switch (Ty->getTypeID()) {
  case Type::IntegerTyID:
    return ConstantInt::get(Ty, 0);
  case Type::FloatTyID:
    return ConstantFP::get(Ty, APFloat(APInt(32, 0)));
  case Type::DoubleTyID:
    return ConstantFP::get(Ty, APFloat(APInt(64, 0)));
  case Type::X86_FP80TyID:
    return ConstantFP::get(Ty, APFloat(APInt(80, 2, zero)));
  case Type::FP128TyID:
    return ConstantFP::get(Ty, APFloat(APInt(128, 2, zero), true));
  case Type::PPC_FP128TyID:
    return ConstantFP::get(Ty, APFloat(APInt(128, 2, zero)));
  case Type::PointerTyID:
    return ConstantPointerNull::get(cast<PointerType>(Ty));
  case Type::StructTyID:
  case Type::ArrayTyID:
  case Type::VectorTyID:
    return ConstantAggregateZero::get(Ty);
  default:
    // Function, Label, or Opaque type?
    assert(!"Cannot create a null constant of that type!");
    return 0;
  }
}

Constant *Constant::getAllOnesValue(const Type *Ty) {
  if (const IntegerType* ITy = dyn_cast<IntegerType>(Ty))
    return ConstantInt::get(APInt::getAllOnesValue(ITy->getBitWidth()));
  return ConstantVector::getAllOnesValue(cast<VectorType>(Ty));
}

// Static constructor to create an integral constant with all bits set
ConstantInt *ConstantInt::getAllOnesValue(const Type *Ty) {
  if (const IntegerType* ITy = dyn_cast<IntegerType>(Ty))
    return ConstantInt::get(APInt::getAllOnesValue(ITy->getBitWidth()));
  return 0;
}

/// @returns the value for a vector integer constant of the given type that
/// has all its bits set to true.
/// @brief Get the all ones value
ConstantVector *ConstantVector::getAllOnesValue(const VectorType *Ty) {
  std::vector<Constant*> Elts;
  Elts.resize(Ty->getNumElements(),
              ConstantInt::getAllOnesValue(Ty->getElementType()));
  assert(Elts[0] && "Not a vector integer type!");
  return cast<ConstantVector>(ConstantVector::get(Elts));
}


//===----------------------------------------------------------------------===//
//                                ConstantInt
//===----------------------------------------------------------------------===//

ConstantInt::ConstantInt(const IntegerType *Ty, const APInt& V)
  : Constant(Ty, ConstantIntVal, 0, 0), Val(V) {
  assert(V.getBitWidth() == Ty->getBitWidth() && "Invalid constant for type");
}

ConstantInt *ConstantInt::TheTrueVal = 0;
ConstantInt *ConstantInt::TheFalseVal = 0;

namespace llvm {
  void CleanupTrueFalse(void *) {
    ConstantInt::ResetTrueFalse();
  }
}

static ManagedCleanup<llvm::CleanupTrueFalse> TrueFalseCleanup;

ConstantInt *ConstantInt::CreateTrueFalseVals(bool WhichOne) {
  assert(TheTrueVal == 0 && TheFalseVal == 0);
  TheTrueVal  = get(Type::Int1Ty, 1);
  TheFalseVal = get(Type::Int1Ty, 0);
  
  // Ensure that llvm_shutdown nulls out TheTrueVal/TheFalseVal.
  TrueFalseCleanup.Register();
  
  return WhichOne ? TheTrueVal : TheFalseVal;
}


namespace {
  struct DenseMapAPIntKeyInfo {
    struct KeyTy {
      APInt val;
      const Type* type;
      KeyTy(const APInt& V, const Type* Ty) : val(V), type(Ty) {}
      KeyTy(const KeyTy& that) : val(that.val), type(that.type) {}
      bool operator==(const KeyTy& that) const {
        return type == that.type && this->val == that.val;
      }
      bool operator!=(const KeyTy& that) const {
        return !this->operator==(that);
      }
    };
    static inline KeyTy getEmptyKey() { return KeyTy(APInt(1,0), 0); }
    static inline KeyTy getTombstoneKey() { return KeyTy(APInt(1,1), 0); }
    static unsigned getHashValue(const KeyTy &Key) {
      return DenseMapInfo<void*>::getHashValue(Key.type) ^ 
        Key.val.getHashValue();
    }
    static bool isEqual(const KeyTy &LHS, const KeyTy &RHS) {
      return LHS == RHS;
    }
    static bool isPod() { return false; }
  };
}


typedef DenseMap<DenseMapAPIntKeyInfo::KeyTy, ConstantInt*, 
                 DenseMapAPIntKeyInfo> IntMapTy;
static ManagedStatic<IntMapTy> IntConstants;

ConstantInt *ConstantInt::get(const Type *Ty, uint64_t V, bool isSigned) {
  const IntegerType *ITy = cast<IntegerType>(Ty);
  return get(APInt(ITy->getBitWidth(), V, isSigned));
}

// Get a ConstantInt from an APInt. Note that the value stored in the DenseMap 
// as the key, is a DensMapAPIntKeyInfo::KeyTy which has provided the
// operator== and operator!= to ensure that the DenseMap doesn't attempt to
// compare APInt's of different widths, which would violate an APInt class
// invariant which generates an assertion.
ConstantInt *ConstantInt::get(const APInt& V) {
  // Get the corresponding integer type for the bit width of the value.
  const IntegerType *ITy = IntegerType::get(V.getBitWidth());
  // get an existing value or the insertion position
  DenseMapAPIntKeyInfo::KeyTy Key(V, ITy);
  ConstantInt *&Slot = (*IntConstants)[Key]; 
  // if it exists, return it.
  if (Slot)
    return Slot;
  // otherwise create a new one, insert it, and return it.
  return Slot = new ConstantInt(ITy, V);
}

//===----------------------------------------------------------------------===//
//                                ConstantFP
//===----------------------------------------------------------------------===//

ConstantFP::ConstantFP(const Type *Ty, const APFloat& V)
  : Constant(Ty, ConstantFPVal, 0, 0), Val(V) {
  // temporary
  if (Ty==Type::FloatTy)
    assert(&V.getSemantics()==&APFloat::IEEEsingle);
  else if (Ty==Type::DoubleTy)
    assert(&V.getSemantics()==&APFloat::IEEEdouble);
  else if (Ty==Type::X86_FP80Ty)
    assert(&V.getSemantics()==&APFloat::x87DoubleExtended);
  else if (Ty==Type::FP128Ty)
    assert(&V.getSemantics()==&APFloat::IEEEquad);
  else if (Ty==Type::PPC_FP128Ty)
    assert(&V.getSemantics()==&APFloat::PPCDoubleDouble);
  else
    assert(0);
}

bool ConstantFP::isNullValue() const {
  return Val.isZero() && !Val.isNegative();
}

ConstantFP *ConstantFP::getNegativeZero(const Type *Ty) {
  APFloat apf = cast <ConstantFP>(Constant::getNullValue(Ty))->getValueAPF();
  apf.changeSign();
  return ConstantFP::get(Ty, apf);
}

bool ConstantFP::isExactlyValue(const APFloat& V) const {
  return Val.bitwiseIsEqual(V);
}

namespace {
  struct DenseMapAPFloatKeyInfo {
    struct KeyTy {
      APFloat val;
      KeyTy(const APFloat& V) : val(V){}
      KeyTy(const KeyTy& that) : val(that.val) {}
      bool operator==(const KeyTy& that) const {
        return this->val.bitwiseIsEqual(that.val);
      }
      bool operator!=(const KeyTy& that) const {
        return !this->operator==(that);
      }
    };
    static inline KeyTy getEmptyKey() { 
      return KeyTy(APFloat(APFloat::Bogus,1));
    }
    static inline KeyTy getTombstoneKey() { 
      return KeyTy(APFloat(APFloat::Bogus,2)); 
    }
    static unsigned getHashValue(const KeyTy &Key) {
      return Key.val.getHashValue();
    }
    static bool isEqual(const KeyTy &LHS, const KeyTy &RHS) {
      return LHS == RHS;
    }
    static bool isPod() { return false; }
  };
}

//---- ConstantFP::get() implementation...
//
typedef DenseMap<DenseMapAPFloatKeyInfo::KeyTy, ConstantFP*, 
                 DenseMapAPFloatKeyInfo> FPMapTy;

static ManagedStatic<FPMapTy> FPConstants;

ConstantFP *ConstantFP::get(const Type *Ty, const APFloat& V) {
  // temporary
  if (Ty==Type::FloatTy)
    assert(&V.getSemantics()==&APFloat::IEEEsingle);
  else if (Ty==Type::DoubleTy)
    assert(&V.getSemantics()==&APFloat::IEEEdouble);
  else if (Ty==Type::X86_FP80Ty)
    assert(&V.getSemantics()==&APFloat::x87DoubleExtended);
  else if (Ty==Type::FP128Ty)
    assert(&V.getSemantics()==&APFloat::IEEEquad);
  else if (Ty==Type::PPC_FP128Ty)
    assert(&V.getSemantics()==&APFloat::PPCDoubleDouble);
  else
    assert(0);
  
  DenseMapAPFloatKeyInfo::KeyTy Key(V);
  ConstantFP *&Slot = (*FPConstants)[Key];
  if (Slot) return Slot;
  return Slot = new ConstantFP(Ty, V);
}

//===----------------------------------------------------------------------===//
//                            ConstantXXX Classes
//===----------------------------------------------------------------------===//


ConstantArray::ConstantArray(const ArrayType *T,
                             const std::vector<Constant*> &V)
  : Constant(T, ConstantArrayVal, new Use[V.size()], V.size()) {
  assert(V.size() == T->getNumElements() &&
         "Invalid initializer vector for constant array");
  Use *OL = OperandList;
  for (std::vector<Constant*>::const_iterator I = V.begin(), E = V.end();
       I != E; ++I, ++OL) {
    Constant *C = *I;
    assert((C->getType() == T->getElementType() ||
            (T->isAbstract() &&
             C->getType()->getTypeID() == T->getElementType()->getTypeID())) &&
           "Initializer for array element doesn't match array element type!");
    OL->init(C, this);
  }
}

ConstantArray::~ConstantArray() {
  delete [] OperandList;
}

ConstantStruct::ConstantStruct(const StructType *T,
                               const std::vector<Constant*> &V)
  : Constant(T, ConstantStructVal, new Use[V.size()], V.size()) {
  assert(V.size() == T->getNumElements() &&
         "Invalid initializer vector for constant structure");
  Use *OL = OperandList;
  for (std::vector<Constant*>::const_iterator I = V.begin(), E = V.end();
       I != E; ++I, ++OL) {
    Constant *C = *I;
    assert((C->getType() == T->getElementType(I-V.begin()) ||
            ((T->getElementType(I-V.begin())->isAbstract() ||
              C->getType()->isAbstract()) &&
             T->getElementType(I-V.begin())->getTypeID() == 
                   C->getType()->getTypeID())) &&
           "Initializer for struct element doesn't match struct element type!");
    OL->init(C, this);
  }
}

ConstantStruct::~ConstantStruct() {
  delete [] OperandList;
}


ConstantVector::ConstantVector(const VectorType *T,
                               const std::vector<Constant*> &V)
  : Constant(T, ConstantVectorVal, new Use[V.size()], V.size()) {
  Use *OL = OperandList;
    for (std::vector<Constant*>::const_iterator I = V.begin(), E = V.end();
         I != E; ++I, ++OL) {
      Constant *C = *I;
      assert((C->getType() == T->getElementType() ||
            (T->isAbstract() &&
             C->getType()->getTypeID() == T->getElementType()->getTypeID())) &&
           "Initializer for vector element doesn't match vector element type!");
    OL->init(C, this);
  }
}

ConstantVector::~ConstantVector() {
  delete [] OperandList;
}

// We declare several classes private to this file, so use an anonymous
// namespace
namespace {

/// UnaryConstantExpr - This class is private to Constants.cpp, and is used
/// behind the scenes to implement unary constant exprs.
class VISIBILITY_HIDDEN UnaryConstantExpr : public ConstantExpr {
  Use Op;
public:
  UnaryConstantExpr(unsigned Opcode, Constant *C, const Type *Ty)
    : ConstantExpr(Ty, Opcode, &Op, 1), Op(C, this) {}
};

/// BinaryConstantExpr - This class is private to Constants.cpp, and is used
/// behind the scenes to implement binary constant exprs.
class VISIBILITY_HIDDEN BinaryConstantExpr : public ConstantExpr {
  Use Ops[2];
public:
  BinaryConstantExpr(unsigned Opcode, Constant *C1, Constant *C2)
    : ConstantExpr(C1->getType(), Opcode, Ops, 2) {
    Ops[0].init(C1, this);
    Ops[1].init(C2, this);
  }
};

/// SelectConstantExpr - This class is private to Constants.cpp, and is used
/// behind the scenes to implement select constant exprs.
class VISIBILITY_HIDDEN SelectConstantExpr : public ConstantExpr {
  Use Ops[3];
public:
  SelectConstantExpr(Constant *C1, Constant *C2, Constant *C3)
    : ConstantExpr(C2->getType(), Instruction::Select, Ops, 3) {
    Ops[0].init(C1, this);
    Ops[1].init(C2, this);
    Ops[2].init(C3, this);
  }
};

/// ExtractElementConstantExpr - This class is private to
/// Constants.cpp, and is used behind the scenes to implement
/// extractelement constant exprs.
class VISIBILITY_HIDDEN ExtractElementConstantExpr : public ConstantExpr {
  Use Ops[2];
public:
  ExtractElementConstantExpr(Constant *C1, Constant *C2)
    : ConstantExpr(cast<VectorType>(C1->getType())->getElementType(), 
                   Instruction::ExtractElement, Ops, 2) {
    Ops[0].init(C1, this);
    Ops[1].init(C2, this);
  }
};

/// InsertElementConstantExpr - This class is private to
/// Constants.cpp, and is used behind the scenes to implement
/// insertelement constant exprs.
class VISIBILITY_HIDDEN InsertElementConstantExpr : public ConstantExpr {
  Use Ops[3];
public:
  InsertElementConstantExpr(Constant *C1, Constant *C2, Constant *C3)
    : ConstantExpr(C1->getType(), Instruction::InsertElement, 
                   Ops, 3) {
    Ops[0].init(C1, this);
    Ops[1].init(C2, this);
    Ops[2].init(C3, this);
  }
};

/// ShuffleVectorConstantExpr - This class is private to
/// Constants.cpp, and is used behind the scenes to implement
/// shufflevector constant exprs.
class VISIBILITY_HIDDEN ShuffleVectorConstantExpr : public ConstantExpr {
  Use Ops[3];
public:
  ShuffleVectorConstantExpr(Constant *C1, Constant *C2, Constant *C3)
  : ConstantExpr(C1->getType(), Instruction::ShuffleVector, 
                 Ops, 3) {
    Ops[0].init(C1, this);
    Ops[1].init(C2, this);
    Ops[2].init(C3, this);
  }
};

/// GetElementPtrConstantExpr - This class is private to Constants.cpp, and is
/// used behind the scenes to implement getelementpr constant exprs.
struct VISIBILITY_HIDDEN GetElementPtrConstantExpr : public ConstantExpr {
  GetElementPtrConstantExpr(Constant *C, const std::vector<Constant*> &IdxList,
                            const Type *DestTy)
    : ConstantExpr(DestTy, Instruction::GetElementPtr,
                   new Use[IdxList.size()+1], IdxList.size()+1) {
    OperandList[0].init(C, this);
    for (unsigned i = 0, E = IdxList.size(); i != E; ++i)
      OperandList[i+1].init(IdxList[i], this);
  }
  ~GetElementPtrConstantExpr() {
    delete [] OperandList;
  }
};

// CompareConstantExpr - This class is private to Constants.cpp, and is used
// behind the scenes to implement ICmp and FCmp constant expressions. This is
// needed in order to store the predicate value for these instructions.
struct VISIBILITY_HIDDEN CompareConstantExpr : public ConstantExpr {
  unsigned short predicate;
  Use Ops[2];
  CompareConstantExpr(Instruction::OtherOps opc, unsigned short pred, 
                      Constant* LHS, Constant* RHS)
    : ConstantExpr(Type::Int1Ty, opc, Ops, 2), predicate(pred) {
    OperandList[0].init(LHS, this);
    OperandList[1].init(RHS, this);
  }
};

} // end anonymous namespace


// Utility function for determining if a ConstantExpr is a CastOp or not. This
// can't be inline because we don't want to #include Instruction.h into
// Constant.h
bool ConstantExpr::isCast() const {
  return Instruction::isCast(getOpcode());
}

bool ConstantExpr::isCompare() const {
  return getOpcode() == Instruction::ICmp || getOpcode() == Instruction::FCmp;
}

/// ConstantExpr::get* - Return some common constants without having to
/// specify the full Instruction::OPCODE identifier.
///
Constant *ConstantExpr::getNeg(Constant *C) {
  return get(Instruction::Sub,
             ConstantExpr::getZeroValueForNegationExpr(C->getType()),
             C);
}
Constant *ConstantExpr::getNot(Constant *C) {
  assert(isa<IntegerType>(C->getType()) && "Cannot NOT a nonintegral value!");
  return get(Instruction::Xor, C,
             ConstantInt::getAllOnesValue(C->getType()));
}
Constant *ConstantExpr::getAdd(Constant *C1, Constant *C2) {
  return get(Instruction::Add, C1, C2);
}
Constant *ConstantExpr::getSub(Constant *C1, Constant *C2) {
  return get(Instruction::Sub, C1, C2);
}
Constant *ConstantExpr::getMul(Constant *C1, Constant *C2) {
  return get(Instruction::Mul, C1, C2);
}
Constant *ConstantExpr::getUDiv(Constant *C1, Constant *C2) {
  return get(Instruction::UDiv, C1, C2);
}
Constant *ConstantExpr::getSDiv(Constant *C1, Constant *C2) {
  return get(Instruction::SDiv, C1, C2);
}
Constant *ConstantExpr::getFDiv(Constant *C1, Constant *C2) {
  return get(Instruction::FDiv, C1, C2);
}
Constant *ConstantExpr::getURem(Constant *C1, Constant *C2) {
  return get(Instruction::URem, C1, C2);
}
Constant *ConstantExpr::getSRem(Constant *C1, Constant *C2) {
  return get(Instruction::SRem, C1, C2);
}
Constant *ConstantExpr::getFRem(Constant *C1, Constant *C2) {
  return get(Instruction::FRem, C1, C2);
}
Constant *ConstantExpr::getAnd(Constant *C1, Constant *C2) {
  return get(Instruction::And, C1, C2);
}
Constant *ConstantExpr::getOr(Constant *C1, Constant *C2) {
  return get(Instruction::Or, C1, C2);
}
Constant *ConstantExpr::getXor(Constant *C1, Constant *C2) {
  return get(Instruction::Xor, C1, C2);
}
unsigned ConstantExpr::getPredicate() const {
  assert(getOpcode() == Instruction::FCmp || getOpcode() == Instruction::ICmp);
  return dynamic_cast<const CompareConstantExpr*>(this)->predicate;
}
Constant *ConstantExpr::getShl(Constant *C1, Constant *C2) {
  return get(Instruction::Shl, C1, C2);
}
Constant *ConstantExpr::getLShr(Constant *C1, Constant *C2) {
  return get(Instruction::LShr, C1, C2);
}
Constant *ConstantExpr::getAShr(Constant *C1, Constant *C2) {
  return get(Instruction::AShr, C1, C2);
}

/// getWithOperandReplaced - Return a constant expression identical to this
/// one, but with the specified operand set to the specified value.
Constant *
ConstantExpr::getWithOperandReplaced(unsigned OpNo, Constant *Op) const {
  assert(OpNo < getNumOperands() && "Operand num is out of range!");
  assert(Op->getType() == getOperand(OpNo)->getType() &&
         "Replacing operand with value of different type!");
  if (getOperand(OpNo) == Op)
    return const_cast<ConstantExpr*>(this);
  
  Constant *Op0, *Op1, *Op2;
  switch (getOpcode()) {
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::UIToFP:
  case Instruction::SIToFP:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::BitCast:
    return ConstantExpr::getCast(getOpcode(), Op, getType());
  case Instruction::Select:
    Op0 = (OpNo == 0) ? Op : getOperand(0);
    Op1 = (OpNo == 1) ? Op : getOperand(1);
    Op2 = (OpNo == 2) ? Op : getOperand(2);
    return ConstantExpr::getSelect(Op0, Op1, Op2);
  case Instruction::InsertElement:
    Op0 = (OpNo == 0) ? Op : getOperand(0);
    Op1 = (OpNo == 1) ? Op : getOperand(1);
    Op2 = (OpNo == 2) ? Op : getOperand(2);
    return ConstantExpr::getInsertElement(Op0, Op1, Op2);
  case Instruction::ExtractElement:
    Op0 = (OpNo == 0) ? Op : getOperand(0);
    Op1 = (OpNo == 1) ? Op : getOperand(1);
    return ConstantExpr::getExtractElement(Op0, Op1);
  case Instruction::ShuffleVector:
    Op0 = (OpNo == 0) ? Op : getOperand(0);
    Op1 = (OpNo == 1) ? Op : getOperand(1);
    Op2 = (OpNo == 2) ? Op : getOperand(2);
    return ConstantExpr::getShuffleVector(Op0, Op1, Op2);
  case Instruction::GetElementPtr: {
    SmallVector<Constant*, 8> Ops;
    Ops.resize(getNumOperands());
    for (unsigned i = 1, e = getNumOperands(); i != e; ++i)
      Ops[i] = getOperand(i);
    if (OpNo == 0)
      return ConstantExpr::getGetElementPtr(Op, &Ops[0], Ops.size());
    Ops[OpNo-1] = Op;
    return ConstantExpr::getGetElementPtr(getOperand(0), &Ops[0], Ops.size());
  }
  default:
    assert(getNumOperands() == 2 && "Must be binary operator?");
    Op0 = (OpNo == 0) ? Op : getOperand(0);
    Op1 = (OpNo == 1) ? Op : getOperand(1);
    return ConstantExpr::get(getOpcode(), Op0, Op1);
  }
}

/// getWithOperands - This returns the current constant expression with the
/// operands replaced with the specified values.  The specified operands must
/// match count and type with the existing ones.
Constant *ConstantExpr::
getWithOperands(const std::vector<Constant*> &Ops) const {
  assert(Ops.size() == getNumOperands() && "Operand count mismatch!");
  bool AnyChange = false;
  for (unsigned i = 0, e = Ops.size(); i != e; ++i) {
    assert(Ops[i]->getType() == getOperand(i)->getType() &&
           "Operand type mismatch!");
    AnyChange |= Ops[i] != getOperand(i);
  }
  if (!AnyChange)  // No operands changed, return self.
    return const_cast<ConstantExpr*>(this);

  switch (getOpcode()) {
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::UIToFP:
  case Instruction::SIToFP:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::BitCast:
    return ConstantExpr::getCast(getOpcode(), Ops[0], getType());
  case Instruction::Select:
    return ConstantExpr::getSelect(Ops[0], Ops[1], Ops[2]);
  case Instruction::InsertElement:
    return ConstantExpr::getInsertElement(Ops[0], Ops[1], Ops[2]);
  case Instruction::ExtractElement:
    return ConstantExpr::getExtractElement(Ops[0], Ops[1]);
  case Instruction::ShuffleVector:
    return ConstantExpr::getShuffleVector(Ops[0], Ops[1], Ops[2]);
  case Instruction::GetElementPtr:
    return ConstantExpr::getGetElementPtr(Ops[0], &Ops[1], Ops.size()-1);
  case Instruction::ICmp:
  case Instruction::FCmp:
    return ConstantExpr::getCompare(getPredicate(), Ops[0], Ops[1]);
  default:
    assert(getNumOperands() == 2 && "Must be binary operator?");
    return ConstantExpr::get(getOpcode(), Ops[0], Ops[1]);
  }
}


//===----------------------------------------------------------------------===//
//                      isValueValidForType implementations

bool ConstantInt::isValueValidForType(const Type *Ty, uint64_t Val) {
  unsigned NumBits = cast<IntegerType>(Ty)->getBitWidth(); // assert okay
  if (Ty == Type::Int1Ty)
    return Val == 0 || Val == 1;
  if (NumBits >= 64)
    return true; // always true, has to fit in largest type
  uint64_t Max = (1ll << NumBits) - 1;
  return Val <= Max;
}

bool ConstantInt::isValueValidForType(const Type *Ty, int64_t Val) {
  unsigned NumBits = cast<IntegerType>(Ty)->getBitWidth(); // assert okay
  if (Ty == Type::Int1Ty)
    return Val == 0 || Val == 1 || Val == -1;
  if (NumBits >= 64)
    return true; // always true, has to fit in largest type
  int64_t Min = -(1ll << (NumBits-1));
  int64_t Max = (1ll << (NumBits-1)) - 1;
  return (Val >= Min && Val <= Max);
}

bool ConstantFP::isValueValidForType(const Type *Ty, const APFloat& Val) {
  // convert modifies in place, so make a copy.
  APFloat Val2 = APFloat(Val);
  switch (Ty->getTypeID()) {
  default:
    return false;         // These can't be represented as floating point!

  // FIXME rounding mode needs to be more flexible
  case Type::FloatTyID:
    return &Val2.getSemantics() == &APFloat::IEEEsingle ||
           Val2.convert(APFloat::IEEEsingle, APFloat::rmNearestTiesToEven) == 
              APFloat::opOK;
  case Type::DoubleTyID:
    return &Val2.getSemantics() == &APFloat::IEEEsingle || 
           &Val2.getSemantics() == &APFloat::IEEEdouble ||
           Val2.convert(APFloat::IEEEdouble, APFloat::rmNearestTiesToEven) == 
             APFloat::opOK;
  case Type::X86_FP80TyID:
    return &Val2.getSemantics() == &APFloat::IEEEsingle || 
           &Val2.getSemantics() == &APFloat::IEEEdouble ||
           &Val2.getSemantics() == &APFloat::x87DoubleExtended;
  case Type::FP128TyID:
    return &Val2.getSemantics() == &APFloat::IEEEsingle || 
           &Val2.getSemantics() == &APFloat::IEEEdouble ||
           &Val2.getSemantics() == &APFloat::IEEEquad;
  case Type::PPC_FP128TyID:
    return &Val2.getSemantics() == &APFloat::IEEEsingle || 
           &Val2.getSemantics() == &APFloat::IEEEdouble ||
           &Val2.getSemantics() == &APFloat::PPCDoubleDouble;
  }
}

//===----------------------------------------------------------------------===//
//                      Factory Function Implementation

// ConstantCreator - A class that is used to create constants by
// ValueMap*.  This class should be partially specialized if there is
// something strange that needs to be done to interface to the ctor for the
// constant.
//
namespace llvm {
  template<class ConstantClass, class TypeClass, class ValType>
  struct VISIBILITY_HIDDEN ConstantCreator {
    static ConstantClass *create(const TypeClass *Ty, const ValType &V) {
      return new ConstantClass(Ty, V);
    }
  };

  template<class ConstantClass, class TypeClass>
  struct VISIBILITY_HIDDEN ConvertConstantType {
    static void convert(ConstantClass *OldC, const TypeClass *NewTy) {
      assert(0 && "This type cannot be converted!\n");
      abort();
    }
  };

  template<class ValType, class TypeClass, class ConstantClass,
           bool HasLargeKey = false  /*true for arrays and structs*/ >
  class VISIBILITY_HIDDEN ValueMap : public AbstractTypeUser {
  public:
    typedef std::pair<const Type*, ValType> MapKey;
    typedef std::map<MapKey, Constant *> MapTy;
    typedef std::map<Constant*, typename MapTy::iterator> InverseMapTy;
    typedef std::map<const Type*, typename MapTy::iterator> AbstractTypeMapTy;
  private:
    /// Map - This is the main map from the element descriptor to the Constants.
    /// This is the primary way we avoid creating two of the same shape
    /// constant.
    MapTy Map;
    
    /// InverseMap - If "HasLargeKey" is true, this contains an inverse mapping
    /// from the constants to their element in Map.  This is important for
    /// removal of constants from the array, which would otherwise have to scan
    /// through the map with very large keys.
    InverseMapTy InverseMap;

    /// AbstractTypeMap - Map for abstract type constants.
    ///
    AbstractTypeMapTy AbstractTypeMap;

  public:
    typename MapTy::iterator map_end() { return Map.end(); }
    
    /// InsertOrGetItem - Return an iterator for the specified element.
    /// If the element exists in the map, the returned iterator points to the
    /// entry and Exists=true.  If not, the iterator points to the newly
    /// inserted entry and returns Exists=false.  Newly inserted entries have
    /// I->second == 0, and should be filled in.
    typename MapTy::iterator InsertOrGetItem(std::pair<MapKey, Constant *>
                                   &InsertVal,
                                   bool &Exists) {
      std::pair<typename MapTy::iterator, bool> IP = Map.insert(InsertVal);
      Exists = !IP.second;
      return IP.first;
    }
    
private:
    typename MapTy::iterator FindExistingElement(ConstantClass *CP) {
      if (HasLargeKey) {
        typename InverseMapTy::iterator IMI = InverseMap.find(CP);
        assert(IMI != InverseMap.end() && IMI->second != Map.end() &&
               IMI->second->second == CP &&
               "InverseMap corrupt!");
        return IMI->second;
      }
      
      typename MapTy::iterator I =
        Map.find(MapKey((TypeClass*)CP->getRawType(), getValType(CP)));
      if (I == Map.end() || I->second != CP) {
        // FIXME: This should not use a linear scan.  If this gets to be a
        // performance problem, someone should look at this.
        for (I = Map.begin(); I != Map.end() && I->second != CP; ++I)
          /* empty */;
      }
      return I;
    }
public:
    
    /// getOrCreate - Return the specified constant from the map, creating it if
    /// necessary.
    ConstantClass *getOrCreate(const TypeClass *Ty, const ValType &V) {
      MapKey Lookup(Ty, V);
      typename MapTy::iterator I = Map.lower_bound(Lookup);
      // Is it in the map?      
      if (I != Map.end() && I->first == Lookup)
        return static_cast<ConstantClass *>(I->second);  

      // If no preexisting value, create one now...
      ConstantClass *Result =
        ConstantCreator<ConstantClass,TypeClass,ValType>::create(Ty, V);

      /// FIXME: why does this assert fail when loading 176.gcc?
      //assert(Result->getType() == Ty && "Type specified is not correct!");
      I = Map.insert(I, std::make_pair(MapKey(Ty, V), Result));

      if (HasLargeKey)  // Remember the reverse mapping if needed.
        InverseMap.insert(std::make_pair(Result, I));
      
      // If the type of the constant is abstract, make sure that an entry exists
      // for it in the AbstractTypeMap.
      if (Ty->isAbstract()) {
        typename AbstractTypeMapTy::iterator TI =
          AbstractTypeMap.lower_bound(Ty);

        if (TI == AbstractTypeMap.end() || TI->first != Ty) {
          // Add ourselves to the ATU list of the type.
          cast<DerivedType>(Ty)->addAbstractTypeUser(this);

          AbstractTypeMap.insert(TI, std::make_pair(Ty, I));
        }
      }
      return Result;
    }

    void remove(ConstantClass *CP) {
      typename MapTy::iterator I = FindExistingElement(CP);
      assert(I != Map.end() && "Constant not found in constant table!");
      assert(I->second == CP && "Didn't find correct element?");

      if (HasLargeKey)  // Remember the reverse mapping if needed.
        InverseMap.erase(CP);
      
      // Now that we found the entry, make sure this isn't the entry that
      // the AbstractTypeMap points to.
      const TypeClass *Ty = static_cast<const TypeClass *>(I->first.first);
      if (Ty->isAbstract()) {
        assert(AbstractTypeMap.count(Ty) &&
               "Abstract type not in AbstractTypeMap?");
        typename MapTy::iterator &ATMEntryIt = AbstractTypeMap[Ty];
        if (ATMEntryIt == I) {
          // Yes, we are removing the representative entry for this type.
          // See if there are any other entries of the same type.
          typename MapTy::iterator TmpIt = ATMEntryIt;

          // First check the entry before this one...
          if (TmpIt != Map.begin()) {
            --TmpIt;
            if (TmpIt->first.first != Ty) // Not the same type, move back...
              ++TmpIt;
          }

          // If we didn't find the same type, try to move forward...
          if (TmpIt == ATMEntryIt) {
            ++TmpIt;
            if (TmpIt == Map.end() || TmpIt->first.first != Ty)
              --TmpIt;   // No entry afterwards with the same type
          }

          // If there is another entry in the map of the same abstract type,
          // update the AbstractTypeMap entry now.
          if (TmpIt != ATMEntryIt) {
            ATMEntryIt = TmpIt;
          } else {
            // Otherwise, we are removing the last instance of this type
            // from the table.  Remove from the ATM, and from user list.
            cast<DerivedType>(Ty)->removeAbstractTypeUser(this);
            AbstractTypeMap.erase(Ty);
          }
        }
      }

      Map.erase(I);
    }

    
    /// MoveConstantToNewSlot - If we are about to change C to be the element
    /// specified by I, update our internal data structures to reflect this
    /// fact.
    void MoveConstantToNewSlot(ConstantClass *C, typename MapTy::iterator I) {
      // First, remove the old location of the specified constant in the map.
      typename MapTy::iterator OldI = FindExistingElement(C);
      assert(OldI != Map.end() && "Constant not found in constant table!");
      assert(OldI->second == C && "Didn't find correct element?");
      
      // If this constant is the representative element for its abstract type,
      // update the AbstractTypeMap so that the representative element is I.
      if (C->getType()->isAbstract()) {
        typename AbstractTypeMapTy::iterator ATI =
            AbstractTypeMap.find(C->getType());
        assert(ATI != AbstractTypeMap.end() &&
               "Abstract type not in AbstractTypeMap?");
        if (ATI->second == OldI)
          ATI->second = I;
      }
      
      // Remove the old entry from the map.
      Map.erase(OldI);
      
      // Update the inverse map so that we know that this constant is now
      // located at descriptor I.
      if (HasLargeKey) {
        assert(I->second == C && "Bad inversemap entry!");
        InverseMap[C] = I;
      }
    }
    
    void refineAbstractType(const DerivedType *OldTy, const Type *NewTy) {
      typename AbstractTypeMapTy::iterator I =
        AbstractTypeMap.find(cast<Type>(OldTy));

      assert(I != AbstractTypeMap.end() &&
             "Abstract type not in AbstractTypeMap?");

      // Convert a constant at a time until the last one is gone.  The last one
      // leaving will remove() itself, causing the AbstractTypeMapEntry to be
      // eliminated eventually.
      do {
        ConvertConstantType<ConstantClass,
                            TypeClass>::convert(
                                static_cast<ConstantClass *>(I->second->second),
                                                cast<TypeClass>(NewTy));

        I = AbstractTypeMap.find(cast<Type>(OldTy));
      } while (I != AbstractTypeMap.end());
    }

    // If the type became concrete without being refined to any other existing
    // type, we just remove ourselves from the ATU list.
    void typeBecameConcrete(const DerivedType *AbsTy) {
      AbsTy->removeAbstractTypeUser(this);
    }

    void dump() const {
      DOUT << "Constant.cpp: ValueMap\n";
    }
  };
}



//---- ConstantAggregateZero::get() implementation...
//
namespace llvm {
  // ConstantAggregateZero does not take extra "value" argument...
  template<class ValType>
  struct ConstantCreator<ConstantAggregateZero, Type, ValType> {
    static ConstantAggregateZero *create(const Type *Ty, const ValType &V){
      return new ConstantAggregateZero(Ty);
    }
  };

  template<>
  struct ConvertConstantType<ConstantAggregateZero, Type> {
    static void convert(ConstantAggregateZero *OldC, const Type *NewTy) {
      // Make everyone now use a constant of the new type...
      Constant *New = ConstantAggregateZero::get(NewTy);
      assert(New != OldC && "Didn't replace constant??");
      OldC->uncheckedReplaceAllUsesWith(New);
      OldC->destroyConstant();     // This constant is now dead, destroy it.
    }
  };
}

static ManagedStatic<ValueMap<char, Type, 
                              ConstantAggregateZero> > AggZeroConstants;

static char getValType(ConstantAggregateZero *CPZ) { return 0; }

Constant *ConstantAggregateZero::get(const Type *Ty) {
  assert((isa<StructType>(Ty) || isa<ArrayType>(Ty) || isa<VectorType>(Ty)) &&
         "Cannot create an aggregate zero of non-aggregate type!");
  return AggZeroConstants->getOrCreate(Ty, 0);
}

// destroyConstant - Remove the constant from the constant table...
//
void ConstantAggregateZero::destroyConstant() {
  AggZeroConstants->remove(this);
  destroyConstantImpl();
}

//---- ConstantArray::get() implementation...
//
namespace llvm {
  template<>
  struct ConvertConstantType<ConstantArray, ArrayType> {
    static void convert(ConstantArray *OldC, const ArrayType *NewTy) {
      // Make everyone now use a constant of the new type...
      std::vector<Constant*> C;
      for (unsigned i = 0, e = OldC->getNumOperands(); i != e; ++i)
        C.push_back(cast<Constant>(OldC->getOperand(i)));
      Constant *New = ConstantArray::get(NewTy, C);
      assert(New != OldC && "Didn't replace constant??");
      OldC->uncheckedReplaceAllUsesWith(New);
      OldC->destroyConstant();    // This constant is now dead, destroy it.
    }
  };
}

static std::vector<Constant*> getValType(ConstantArray *CA) {
  std::vector<Constant*> Elements;
  Elements.reserve(CA->getNumOperands());
  for (unsigned i = 0, e = CA->getNumOperands(); i != e; ++i)
    Elements.push_back(cast<Constant>(CA->getOperand(i)));
  return Elements;
}

typedef ValueMap<std::vector<Constant*>, ArrayType, 
                 ConstantArray, true /*largekey*/> ArrayConstantsTy;
static ManagedStatic<ArrayConstantsTy> ArrayConstants;

Constant *ConstantArray::get(const ArrayType *Ty,
                             const std::vector<Constant*> &V) {
  // If this is an all-zero array, return a ConstantAggregateZero object
  if (!V.empty()) {
    Constant *C = V[0];
    if (!C->isNullValue())
      return ArrayConstants->getOrCreate(Ty, V);
    for (unsigned i = 1, e = V.size(); i != e; ++i)
      if (V[i] != C)
        return ArrayConstants->getOrCreate(Ty, V);
  }
  return ConstantAggregateZero::get(Ty);
}

// destroyConstant - Remove the constant from the constant table...
//
void ConstantArray::destroyConstant() {
  ArrayConstants->remove(this);
  destroyConstantImpl();
}

/// ConstantArray::get(const string&) - Return an array that is initialized to
/// contain the specified string.  If length is zero then a null terminator is 
/// added to the specified string so that it may be used in a natural way. 
/// Otherwise, the length parameter specifies how much of the string to use 
/// and it won't be null terminated.
///
Constant *ConstantArray::get(const std::string &Str, bool AddNull) {
  std::vector<Constant*> ElementVals;
  for (unsigned i = 0; i < Str.length(); ++i)
    ElementVals.push_back(ConstantInt::get(Type::Int8Ty, Str[i]));

  // Add a null terminator to the string...
  if (AddNull) {
    ElementVals.push_back(ConstantInt::get(Type::Int8Ty, 0));
  }

  ArrayType *ATy = ArrayType::get(Type::Int8Ty, ElementVals.size());
  return ConstantArray::get(ATy, ElementVals);
}

/// isString - This method returns true if the array is an array of i8, and 
/// if the elements of the array are all ConstantInt's.
bool ConstantArray::isString() const {
  // Check the element type for i8...
  if (getType()->getElementType() != Type::Int8Ty)
    return false;
  // Check the elements to make sure they are all integers, not constant
  // expressions.
  for (unsigned i = 0, e = getNumOperands(); i != e; ++i)
    if (!isa<ConstantInt>(getOperand(i)))
      return false;
  return true;
}

/// isCString - This method returns true if the array is a string (see
/// isString) and it ends in a null byte \0 and does not contains any other
/// null bytes except its terminator.
bool ConstantArray::isCString() const {
  // Check the element type for i8...
  if (getType()->getElementType() != Type::Int8Ty)
    return false;
  Constant *Zero = Constant::getNullValue(getOperand(0)->getType());
  // Last element must be a null.
  if (getOperand(getNumOperands()-1) != Zero)
    return false;
  // Other elements must be non-null integers.
  for (unsigned i = 0, e = getNumOperands()-1; i != e; ++i) {
    if (!isa<ConstantInt>(getOperand(i)))
      return false;
    if (getOperand(i) == Zero)
      return false;
  }
  return true;
}


// getAsString - If the sub-element type of this array is i8
// then this method converts the array to an std::string and returns it.
// Otherwise, it asserts out.
//
std::string ConstantArray::getAsString() const {
  assert(isString() && "Not a string!");
  std::string Result;
  for (unsigned i = 0, e = getNumOperands(); i != e; ++i)
    Result += (char)cast<ConstantInt>(getOperand(i))->getZExtValue();
  return Result;
}


//---- ConstantStruct::get() implementation...
//

namespace llvm {
  template<>
  struct ConvertConstantType<ConstantStruct, StructType> {
    static void convert(ConstantStruct *OldC, const StructType *NewTy) {
      // Make everyone now use a constant of the new type...
      std::vector<Constant*> C;
      for (unsigned i = 0, e = OldC->getNumOperands(); i != e; ++i)
        C.push_back(cast<Constant>(OldC->getOperand(i)));
      Constant *New = ConstantStruct::get(NewTy, C);
      assert(New != OldC && "Didn't replace constant??");

      OldC->uncheckedReplaceAllUsesWith(New);
      OldC->destroyConstant();    // This constant is now dead, destroy it.
    }
  };
}

typedef ValueMap<std::vector<Constant*>, StructType,
                 ConstantStruct, true /*largekey*/> StructConstantsTy;
static ManagedStatic<StructConstantsTy> StructConstants;

static std::vector<Constant*> getValType(ConstantStruct *CS) {
  std::vector<Constant*> Elements;
  Elements.reserve(CS->getNumOperands());
  for (unsigned i = 0, e = CS->getNumOperands(); i != e; ++i)
    Elements.push_back(cast<Constant>(CS->getOperand(i)));
  return Elements;
}

Constant *ConstantStruct::get(const StructType *Ty,
                              const std::vector<Constant*> &V) {
  // Create a ConstantAggregateZero value if all elements are zeros...
  for (unsigned i = 0, e = V.size(); i != e; ++i)
    if (!V[i]->isNullValue())
      return StructConstants->getOrCreate(Ty, V);

  return ConstantAggregateZero::get(Ty);
}

Constant *ConstantStruct::get(const std::vector<Constant*> &V, bool packed) {
  std::vector<const Type*> StructEls;
  StructEls.reserve(V.size());
  for (unsigned i = 0, e = V.size(); i != e; ++i)
    StructEls.push_back(V[i]->getType());
  return get(StructType::get(StructEls, packed), V);
}

// destroyConstant - Remove the constant from the constant table...
//
void ConstantStruct::destroyConstant() {
  StructConstants->remove(this);
  destroyConstantImpl();
}

//---- ConstantVector::get() implementation...
//
namespace llvm {
  template<>
  struct ConvertConstantType<ConstantVector, VectorType> {
    static void convert(ConstantVector *OldC, const VectorType *NewTy) {
      // Make everyone now use a constant of the new type...
      std::vector<Constant*> C;
      for (unsigned i = 0, e = OldC->getNumOperands(); i != e; ++i)
        C.push_back(cast<Constant>(OldC->getOperand(i)));
      Constant *New = ConstantVector::get(NewTy, C);
      assert(New != OldC && "Didn't replace constant??");
      OldC->uncheckedReplaceAllUsesWith(New);
      OldC->destroyConstant();    // This constant is now dead, destroy it.
    }
  };
}

static std::vector<Constant*> getValType(ConstantVector *CP) {
  std::vector<Constant*> Elements;
  Elements.reserve(CP->getNumOperands());
  for (unsigned i = 0, e = CP->getNumOperands(); i != e; ++i)
    Elements.push_back(CP->getOperand(i));
  return Elements;
}

static ManagedStatic<ValueMap<std::vector<Constant*>, VectorType,
                              ConstantVector> > VectorConstants;

Constant *ConstantVector::get(const VectorType *Ty,
                              const std::vector<Constant*> &V) {
  // If this is an all-zero vector, return a ConstantAggregateZero object
  if (!V.empty()) {
    Constant *C = V[0];
    if (!C->isNullValue())
      return VectorConstants->getOrCreate(Ty, V);
    for (unsigned i = 1, e = V.size(); i != e; ++i)
      if (V[i] != C)
        return VectorConstants->getOrCreate(Ty, V);
  }
  return ConstantAggregateZero::get(Ty);
}

Constant *ConstantVector::get(const std::vector<Constant*> &V) {
  assert(!V.empty() && "Cannot infer type if V is empty");
  return get(VectorType::get(V.front()->getType(),V.size()), V);
}

// destroyConstant - Remove the constant from the constant table...
//
void ConstantVector::destroyConstant() {
  VectorConstants->remove(this);
  destroyConstantImpl();
}

/// This function will return true iff every element in this vector constant
/// is set to all ones.
/// @returns true iff this constant's emements are all set to all ones.
/// @brief Determine if the value is all ones.
bool ConstantVector::isAllOnesValue() const {
  // Check out first element.
  const Constant *Elt = getOperand(0);
  const ConstantInt *CI = dyn_cast<ConstantInt>(Elt);
  if (!CI || !CI->isAllOnesValue()) return false;
  // Then make sure all remaining elements point to the same value.
  for (unsigned I = 1, E = getNumOperands(); I < E; ++I) {
    if (getOperand(I) != Elt) return false;
  }
  return true;
}

//---- ConstantPointerNull::get() implementation...
//

namespace llvm {
  // ConstantPointerNull does not take extra "value" argument...
  template<class ValType>
  struct ConstantCreator<ConstantPointerNull, PointerType, ValType> {
    static ConstantPointerNull *create(const PointerType *Ty, const ValType &V){
      return new ConstantPointerNull(Ty);
    }
  };

  template<>
  struct ConvertConstantType<ConstantPointerNull, PointerType> {
    static void convert(ConstantPointerNull *OldC, const PointerType *NewTy) {
      // Make everyone now use a constant of the new type...
      Constant *New = ConstantPointerNull::get(NewTy);
      assert(New != OldC && "Didn't replace constant??");
      OldC->uncheckedReplaceAllUsesWith(New);
      OldC->destroyConstant();     // This constant is now dead, destroy it.
    }
  };
}

static ManagedStatic<ValueMap<char, PointerType, 
                              ConstantPointerNull> > NullPtrConstants;

static char getValType(ConstantPointerNull *) {
  return 0;
}


ConstantPointerNull *ConstantPointerNull::get(const PointerType *Ty) {
  return NullPtrConstants->getOrCreate(Ty, 0);
}

// destroyConstant - Remove the constant from the constant table...
//
void ConstantPointerNull::destroyConstant() {
  NullPtrConstants->remove(this);
  destroyConstantImpl();
}


//---- UndefValue::get() implementation...
//

namespace llvm {
  // UndefValue does not take extra "value" argument...
  template<class ValType>
  struct ConstantCreator<UndefValue, Type, ValType> {
    static UndefValue *create(const Type *Ty, const ValType &V) {
      return new UndefValue(Ty);
    }
  };

  template<>
  struct ConvertConstantType<UndefValue, Type> {
    static void convert(UndefValue *OldC, const Type *NewTy) {
      // Make everyone now use a constant of the new type.
      Constant *New = UndefValue::get(NewTy);
      assert(New != OldC && "Didn't replace constant??");
      OldC->uncheckedReplaceAllUsesWith(New);
      OldC->destroyConstant();     // This constant is now dead, destroy it.
    }
  };
}

static ManagedStatic<ValueMap<char, Type, UndefValue> > UndefValueConstants;

static char getValType(UndefValue *) {
  return 0;
}


UndefValue *UndefValue::get(const Type *Ty) {
  return UndefValueConstants->getOrCreate(Ty, 0);
}

// destroyConstant - Remove the constant from the constant table.
//
void UndefValue::destroyConstant() {
  UndefValueConstants->remove(this);
  destroyConstantImpl();
}


//---- ConstantExpr::get() implementations...
//

struct ExprMapKeyType {
  explicit ExprMapKeyType(unsigned opc, std::vector<Constant*> ops,
      unsigned short pred = 0) : opcode(opc), predicate(pred), operands(ops) { }
  uint16_t opcode;
  uint16_t predicate;
  std::vector<Constant*> operands;
  bool operator==(const ExprMapKeyType& that) const {
    return this->opcode == that.opcode &&
           this->predicate == that.predicate &&
           this->operands == that.operands;
  }
  bool operator<(const ExprMapKeyType & that) const {
    return this->opcode < that.opcode ||
      (this->opcode == that.opcode && this->predicate < that.predicate) ||
      (this->opcode == that.opcode && this->predicate == that.predicate &&
       this->operands < that.operands);
  }

  bool operator!=(const ExprMapKeyType& that) const {
    return !(*this == that);
  }
};

namespace llvm {
  template<>
  struct ConstantCreator<ConstantExpr, Type, ExprMapKeyType> {
    static ConstantExpr *create(const Type *Ty, const ExprMapKeyType &V,
        unsigned short pred = 0) {
      if (Instruction::isCast(V.opcode))
        return new UnaryConstantExpr(V.opcode, V.operands[0], Ty);
      if ((V.opcode >= Instruction::BinaryOpsBegin &&
           V.opcode < Instruction::BinaryOpsEnd))
        return new BinaryConstantExpr(V.opcode, V.operands[0], V.operands[1]);
      if (V.opcode == Instruction::Select)
        return new SelectConstantExpr(V.operands[0], V.operands[1], 
                                      V.operands[2]);
      if (V.opcode == Instruction::ExtractElement)
        return new ExtractElementConstantExpr(V.operands[0], V.operands[1]);
      if (V.opcode == Instruction::InsertElement)
        return new InsertElementConstantExpr(V.operands[0], V.operands[1],
                                             V.operands[2]);
      if (V.opcode == Instruction::ShuffleVector)
        return new ShuffleVectorConstantExpr(V.operands[0], V.operands[1],
                                             V.operands[2]);
      if (V.opcode == Instruction::GetElementPtr) {
        std::vector<Constant*> IdxList(V.operands.begin()+1, V.operands.end());
        return new GetElementPtrConstantExpr(V.operands[0], IdxList, Ty);
      }

      // The compare instructions are weird. We have to encode the predicate
      // value and it is combined with the instruction opcode by multiplying
      // the opcode by one hundred. We must decode this to get the predicate.
      if (V.opcode == Instruction::ICmp)
        return new CompareConstantExpr(Instruction::ICmp, V.predicate, 
                                       V.operands[0], V.operands[1]);
      if (V.opcode == Instruction::FCmp) 
        return new CompareConstantExpr(Instruction::FCmp, V.predicate, 
                                       V.operands[0], V.operands[1]);
      assert(0 && "Invalid ConstantExpr!");
      return 0;
    }
  };

  template<>
  struct ConvertConstantType<ConstantExpr, Type> {
    static void convert(ConstantExpr *OldC, const Type *NewTy) {
      Constant *New;
      switch (OldC->getOpcode()) {
      case Instruction::Trunc:
      case Instruction::ZExt:
      case Instruction::SExt:
      case Instruction::FPTrunc:
      case Instruction::FPExt:
      case Instruction::UIToFP:
      case Instruction::SIToFP:
      case Instruction::FPToUI:
      case Instruction::FPToSI:
      case Instruction::PtrToInt:
      case Instruction::IntToPtr:
      case Instruction::BitCast:
        New = ConstantExpr::getCast(OldC->getOpcode(), OldC->getOperand(0), 
                                    NewTy);
        break;
      case Instruction::Select:
        New = ConstantExpr::getSelectTy(NewTy, OldC->getOperand(0),
                                        OldC->getOperand(1),
                                        OldC->getOperand(2));
        break;
      default:
        assert(OldC->getOpcode() >= Instruction::BinaryOpsBegin &&
               OldC->getOpcode() <  Instruction::BinaryOpsEnd);
        New = ConstantExpr::getTy(NewTy, OldC->getOpcode(), OldC->getOperand(0),
                                  OldC->getOperand(1));
        break;
      case Instruction::GetElementPtr:
        // Make everyone now use a constant of the new type...
        std::vector<Value*> Idx(OldC->op_begin()+1, OldC->op_end());
        New = ConstantExpr::getGetElementPtrTy(NewTy, OldC->getOperand(0),
                                               &Idx[0], Idx.size());
        break;
      }

      assert(New != OldC && "Didn't replace constant??");
      OldC->uncheckedReplaceAllUsesWith(New);
      OldC->destroyConstant();    // This constant is now dead, destroy it.
    }
  };
} // end namespace llvm


static ExprMapKeyType getValType(ConstantExpr *CE) {
  std::vector<Constant*> Operands;
  Operands.reserve(CE->getNumOperands());
  for (unsigned i = 0, e = CE->getNumOperands(); i != e; ++i)
    Operands.push_back(cast<Constant>(CE->getOperand(i)));
  return ExprMapKeyType(CE->getOpcode(), Operands, 
      CE->isCompare() ? CE->getPredicate() : 0);
}

static ManagedStatic<ValueMap<ExprMapKeyType, Type,
                              ConstantExpr> > ExprConstants;

/// This is a utility function to handle folding of casts and lookup of the
/// cast in the ExprConstants map. It is usedby the various get* methods below.
static inline Constant *getFoldedCast(
  Instruction::CastOps opc, Constant *C, const Type *Ty) {
  assert(Ty->isFirstClassType() && "Cannot cast to an aggregate type!");
  // Fold a few common cases
  if (Constant *FC = ConstantFoldCastInstruction(opc, C, Ty))
    return FC;

  // Look up the constant in the table first to ensure uniqueness
  std::vector<Constant*> argVec(1, C);
  ExprMapKeyType Key(opc, argVec);
  return ExprConstants->getOrCreate(Ty, Key);
}
 
Constant *ConstantExpr::getCast(unsigned oc, Constant *C, const Type *Ty) {
  Instruction::CastOps opc = Instruction::CastOps(oc);
  assert(Instruction::isCast(opc) && "opcode out of range");
  assert(C && Ty && "Null arguments to getCast");
  assert(Ty->isFirstClassType() && "Cannot cast to an aggregate type!");

  switch (opc) {
    default:
      assert(0 && "Invalid cast opcode");
      break;
    case Instruction::Trunc:    return getTrunc(C, Ty);
    case Instruction::ZExt:     return getZExt(C, Ty);
    case Instruction::SExt:     return getSExt(C, Ty);
    case Instruction::FPTrunc:  return getFPTrunc(C, Ty);
    case Instruction::FPExt:    return getFPExtend(C, Ty);
    case Instruction::UIToFP:   return getUIToFP(C, Ty);
    case Instruction::SIToFP:   return getSIToFP(C, Ty);
    case Instruction::FPToUI:   return getFPToUI(C, Ty);
    case Instruction::FPToSI:   return getFPToSI(C, Ty);
    case Instruction::PtrToInt: return getPtrToInt(C, Ty);
    case Instruction::IntToPtr: return getIntToPtr(C, Ty);
    case Instruction::BitCast:  return getBitCast(C, Ty);
  }
  return 0;
} 

Constant *ConstantExpr::getZExtOrBitCast(Constant *C, const Type *Ty) {
  if (C->getType()->getPrimitiveSizeInBits() == Ty->getPrimitiveSizeInBits())
    return getCast(Instruction::BitCast, C, Ty);
  return getCast(Instruction::ZExt, C, Ty);
}

Constant *ConstantExpr::getSExtOrBitCast(Constant *C, const Type *Ty) {
  if (C->getType()->getPrimitiveSizeInBits() == Ty->getPrimitiveSizeInBits())
    return getCast(Instruction::BitCast, C, Ty);
  return getCast(Instruction::SExt, C, Ty);
}

Constant *ConstantExpr::getTruncOrBitCast(Constant *C, const Type *Ty) {
  if (C->getType()->getPrimitiveSizeInBits() == Ty->getPrimitiveSizeInBits())
    return getCast(Instruction::BitCast, C, Ty);
  return getCast(Instruction::Trunc, C, Ty);
}

Constant *ConstantExpr::getPointerCast(Constant *S, const Type *Ty) {
  assert(isa<PointerType>(S->getType()) && "Invalid cast");
  assert((Ty->isInteger() || isa<PointerType>(Ty)) && "Invalid cast");

  if (Ty->isInteger())
    return getCast(Instruction::PtrToInt, S, Ty);
  return getCast(Instruction::BitCast, S, Ty);
}

Constant *ConstantExpr::getIntegerCast(Constant *C, const Type *Ty, 
                                       bool isSigned) {
  assert(C->getType()->isInteger() && Ty->isInteger() && "Invalid cast");
  unsigned SrcBits = C->getType()->getPrimitiveSizeInBits();
  unsigned DstBits = Ty->getPrimitiveSizeInBits();
  Instruction::CastOps opcode =
    (SrcBits == DstBits ? Instruction::BitCast :
     (SrcBits > DstBits ? Instruction::Trunc :
      (isSigned ? Instruction::SExt : Instruction::ZExt)));
  return getCast(opcode, C, Ty);
}

Constant *ConstantExpr::getFPCast(Constant *C, const Type *Ty) {
  assert(C->getType()->isFloatingPoint() && Ty->isFloatingPoint() && 
         "Invalid cast");
  unsigned SrcBits = C->getType()->getPrimitiveSizeInBits();
  unsigned DstBits = Ty->getPrimitiveSizeInBits();
  if (SrcBits == DstBits)
    return C; // Avoid a useless cast
  Instruction::CastOps opcode =
     (SrcBits > DstBits ? Instruction::FPTrunc : Instruction::FPExt);
  return getCast(opcode, C, Ty);
}

Constant *ConstantExpr::getTrunc(Constant *C, const Type *Ty) {
  assert(C->getType()->isInteger() && "Trunc operand must be integer");
  assert(Ty->isInteger() && "Trunc produces only integral");
  assert(C->getType()->getPrimitiveSizeInBits() > Ty->getPrimitiveSizeInBits()&&
         "SrcTy must be larger than DestTy for Trunc!");

  return getFoldedCast(Instruction::Trunc, C, Ty);
}

Constant *ConstantExpr::getSExt(Constant *C, const Type *Ty) {
  assert(C->getType()->isInteger() && "SEXt operand must be integral");
  assert(Ty->isInteger() && "SExt produces only integer");
  assert(C->getType()->getPrimitiveSizeInBits() < Ty->getPrimitiveSizeInBits()&&
         "SrcTy must be smaller than DestTy for SExt!");

  return getFoldedCast(Instruction::SExt, C, Ty);
}

Constant *ConstantExpr::getZExt(Constant *C, const Type *Ty) {
  assert(C->getType()->isInteger() && "ZEXt operand must be integral");
  assert(Ty->isInteger() && "ZExt produces only integer");
  assert(C->getType()->getPrimitiveSizeInBits() < Ty->getPrimitiveSizeInBits()&&
         "SrcTy must be smaller than DestTy for ZExt!");

  return getFoldedCast(Instruction::ZExt, C, Ty);
}

Constant *ConstantExpr::getFPTrunc(Constant *C, const Type *Ty) {
  assert(C->getType()->isFloatingPoint() && Ty->isFloatingPoint() &&
         C->getType()->getPrimitiveSizeInBits() > Ty->getPrimitiveSizeInBits()&&
         "This is an illegal floating point truncation!");
  return getFoldedCast(Instruction::FPTrunc, C, Ty);
}

Constant *ConstantExpr::getFPExtend(Constant *C, const Type *Ty) {
  assert(C->getType()->isFloatingPoint() && Ty->isFloatingPoint() &&
         C->getType()->getPrimitiveSizeInBits() < Ty->getPrimitiveSizeInBits()&&
         "This is an illegal floating point extension!");
  return getFoldedCast(Instruction::FPExt, C, Ty);
}

Constant *ConstantExpr::getUIToFP(Constant *C, const Type *Ty) {
  assert(C->getType()->isInteger() && Ty->isFloatingPoint() &&
         "This is an illegal i32 to floating point cast!");
  return getFoldedCast(Instruction::UIToFP, C, Ty);
}

Constant *ConstantExpr::getSIToFP(Constant *C, const Type *Ty) {
  assert(C->getType()->isInteger() && Ty->isFloatingPoint() &&
         "This is an illegal sint to floating point cast!");
  return getFoldedCast(Instruction::SIToFP, C, Ty);
}

Constant *ConstantExpr::getFPToUI(Constant *C, const Type *Ty) {
  assert(C->getType()->isFloatingPoint() && Ty->isInteger() &&
         "This is an illegal floating point to i32 cast!");
  return getFoldedCast(Instruction::FPToUI, C, Ty);
}

Constant *ConstantExpr::getFPToSI(Constant *C, const Type *Ty) {
  assert(C->getType()->isFloatingPoint() && Ty->isInteger() &&
         "This is an illegal floating point to i32 cast!");
  return getFoldedCast(Instruction::FPToSI, C, Ty);
}

Constant *ConstantExpr::getPtrToInt(Constant *C, const Type *DstTy) {
  assert(isa<PointerType>(C->getType()) && "PtrToInt source must be pointer");
  assert(DstTy->isInteger() && "PtrToInt destination must be integral");
  return getFoldedCast(Instruction::PtrToInt, C, DstTy);
}

Constant *ConstantExpr::getIntToPtr(Constant *C, const Type *DstTy) {
  assert(C->getType()->isInteger() && "IntToPtr source must be integral");
  assert(isa<PointerType>(DstTy) && "IntToPtr destination must be a pointer");
  return getFoldedCast(Instruction::IntToPtr, C, DstTy);
}

Constant *ConstantExpr::getBitCast(Constant *C, const Type *DstTy) {
  // BitCast implies a no-op cast of type only. No bits change.  However, you 
  // can't cast pointers to anything but pointers.
  const Type *SrcTy = C->getType();
  assert((isa<PointerType>(SrcTy) == isa<PointerType>(DstTy)) &&
         "BitCast cannot cast pointer to non-pointer and vice versa");

  // Now we know we're not dealing with mismatched pointer casts (ptr->nonptr
  // or nonptr->ptr). For all the other types, the cast is okay if source and 
  // destination bit widths are identical.
  unsigned SrcBitSize = SrcTy->getPrimitiveSizeInBits();
  unsigned DstBitSize = DstTy->getPrimitiveSizeInBits();
  assert(SrcBitSize == DstBitSize && "BitCast requies types of same width");
  return getFoldedCast(Instruction::BitCast, C, DstTy);
}

Constant *ConstantExpr::getSizeOf(const Type *Ty) {
  // sizeof is implemented as: (i64) gep (Ty*)null, 1
  Constant *GEPIdx = ConstantInt::get(Type::Int32Ty, 1);
  Constant *GEP =
    getGetElementPtr(getNullValue(PointerType::get(Ty)), &GEPIdx, 1);
  return getCast(Instruction::PtrToInt, GEP, Type::Int64Ty);
}

Constant *ConstantExpr::getTy(const Type *ReqTy, unsigned Opcode,
                              Constant *C1, Constant *C2) {
  // Check the operands for consistency first
  assert(Opcode >= Instruction::BinaryOpsBegin &&
         Opcode <  Instruction::BinaryOpsEnd   &&
         "Invalid opcode in binary constant expression");
  assert(C1->getType() == C2->getType() &&
         "Operand types in binary constant expression should match");

  if (ReqTy == C1->getType() || ReqTy == Type::Int1Ty)
    if (Constant *FC = ConstantFoldBinaryInstruction(Opcode, C1, C2))
      return FC;          // Fold a few common cases...

  std::vector<Constant*> argVec(1, C1); argVec.push_back(C2);
  ExprMapKeyType Key(Opcode, argVec);
  return ExprConstants->getOrCreate(ReqTy, Key);
}

Constant *ConstantExpr::getCompareTy(unsigned short predicate,
                                     Constant *C1, Constant *C2) {
  switch (predicate) {
    default: assert(0 && "Invalid CmpInst predicate");
    case FCmpInst::FCMP_FALSE: case FCmpInst::FCMP_OEQ: case FCmpInst::FCMP_OGT:
    case FCmpInst::FCMP_OGE: case FCmpInst::FCMP_OLT: case FCmpInst::FCMP_OLE:
    case FCmpInst::FCMP_ONE: case FCmpInst::FCMP_ORD: case FCmpInst::FCMP_UNO:
    case FCmpInst::FCMP_UEQ: case FCmpInst::FCMP_UGT: case FCmpInst::FCMP_UGE:
    case FCmpInst::FCMP_ULT: case FCmpInst::FCMP_ULE: case FCmpInst::FCMP_UNE:
    case FCmpInst::FCMP_TRUE:
      return getFCmp(predicate, C1, C2);
    case ICmpInst::ICMP_EQ: case ICmpInst::ICMP_NE: case ICmpInst::ICMP_UGT:
    case ICmpInst::ICMP_UGE: case ICmpInst::ICMP_ULT: case ICmpInst::ICMP_ULE:
    case ICmpInst::ICMP_SGT: case ICmpInst::ICMP_SGE: case ICmpInst::ICMP_SLT:
    case ICmpInst::ICMP_SLE:
      return getICmp(predicate, C1, C2);
  }
}

Constant *ConstantExpr::get(unsigned Opcode, Constant *C1, Constant *C2) {
#ifndef NDEBUG
  switch (Opcode) {
  case Instruction::Add: 
  case Instruction::Sub:
  case Instruction::Mul: 
    assert(C1->getType() == C2->getType() && "Op types should be identical!");
    assert((C1->getType()->isInteger() || C1->getType()->isFloatingPoint() ||
            isa<VectorType>(C1->getType())) &&
           "Tried to create an arithmetic operation on a non-arithmetic type!");
    break;
  case Instruction::UDiv: 
  case Instruction::SDiv: 
    assert(C1->getType() == C2->getType() && "Op types should be identical!");
    assert((C1->getType()->isInteger() || (isa<VectorType>(C1->getType()) &&
      cast<VectorType>(C1->getType())->getElementType()->isInteger())) &&
           "Tried to create an arithmetic operation on a non-arithmetic type!");
    break;
  case Instruction::FDiv:
    assert(C1->getType() == C2->getType() && "Op types should be identical!");
    assert((C1->getType()->isFloatingPoint() || (isa<VectorType>(C1->getType())
      && cast<VectorType>(C1->getType())->getElementType()->isFloatingPoint())) 
      && "Tried to create an arithmetic operation on a non-arithmetic type!");
    break;
  case Instruction::URem: 
  case Instruction::SRem: 
    assert(C1->getType() == C2->getType() && "Op types should be identical!");
    assert((C1->getType()->isInteger() || (isa<VectorType>(C1->getType()) &&
      cast<VectorType>(C1->getType())->getElementType()->isInteger())) &&
           "Tried to create an arithmetic operation on a non-arithmetic type!");
    break;
  case Instruction::FRem:
    assert(C1->getType() == C2->getType() && "Op types should be identical!");
    assert((C1->getType()->isFloatingPoint() || (isa<VectorType>(C1->getType())
      && cast<VectorType>(C1->getType())->getElementType()->isFloatingPoint())) 
      && "Tried to create an arithmetic operation on a non-arithmetic type!");
    break;
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
    assert(C1->getType() == C2->getType() && "Op types should be identical!");
    assert((C1->getType()->isInteger() || isa<VectorType>(C1->getType())) &&
           "Tried to create a logical operation on a non-integral type!");
    break;
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    assert(C1->getType() == C2->getType() && "Op types should be identical!");
    assert(C1->getType()->isInteger() &&
           "Tried to create a shift operation on a non-integer type!");
    break;
  default:
    break;
  }
#endif

  return getTy(C1->getType(), Opcode, C1, C2);
}

Constant *ConstantExpr::getCompare(unsigned short pred, 
                            Constant *C1, Constant *C2) {
  assert(C1->getType() == C2->getType() && "Op types should be identical!");
  return getCompareTy(pred, C1, C2);
}

Constant *ConstantExpr::getSelectTy(const Type *ReqTy, Constant *C,
                                    Constant *V1, Constant *V2) {
  assert(C->getType() == Type::Int1Ty && "Select condition must be i1!");
  assert(V1->getType() == V2->getType() && "Select value types must match!");
  assert(V1->getType()->isFirstClassType() && "Cannot select aggregate type!");

  if (ReqTy == V1->getType())
    if (Constant *SC = ConstantFoldSelectInstruction(C, V1, V2))
      return SC;        // Fold common cases

  std::vector<Constant*> argVec(3, C);
  argVec[1] = V1;
  argVec[2] = V2;
  ExprMapKeyType Key(Instruction::Select, argVec);
  return ExprConstants->getOrCreate(ReqTy, Key);
}

Constant *ConstantExpr::getGetElementPtrTy(const Type *ReqTy, Constant *C,
                                           Value* const *Idxs,
                                           unsigned NumIdx) {
  assert(GetElementPtrInst::getIndexedType(C->getType(), Idxs, Idxs+NumIdx, true) &&
         "GEP indices invalid!");

  if (Constant *FC = ConstantFoldGetElementPtr(C, (Constant**)Idxs, NumIdx))
    return FC;          // Fold a few common cases...

  assert(isa<PointerType>(C->getType()) &&
         "Non-pointer type for constant GetElementPtr expression");
  // Look up the constant in the table first to ensure uniqueness
  std::vector<Constant*> ArgVec;
  ArgVec.reserve(NumIdx+1);
  ArgVec.push_back(C);
  for (unsigned i = 0; i != NumIdx; ++i)
    ArgVec.push_back(cast<Constant>(Idxs[i]));
  const ExprMapKeyType Key(Instruction::GetElementPtr, ArgVec);
  return ExprConstants->getOrCreate(ReqTy, Key);
}

Constant *ConstantExpr::getGetElementPtr(Constant *C, Value* const *Idxs,
                                         unsigned NumIdx) {
  // Get the result type of the getelementptr!
  const Type *Ty = 
    GetElementPtrInst::getIndexedType(C->getType(), Idxs, Idxs+NumIdx, true);
  assert(Ty && "GEP indices invalid!");
  return getGetElementPtrTy(PointerType::get(Ty), C, Idxs, NumIdx);
}

Constant *ConstantExpr::getGetElementPtr(Constant *C, Constant* const *Idxs,
                                         unsigned NumIdx) {
  return getGetElementPtr(C, (Value* const *)Idxs, NumIdx);
}


Constant *
ConstantExpr::getICmp(unsigned short pred, Constant* LHS, Constant* RHS) {
  assert(LHS->getType() == RHS->getType());
  assert(pred >= ICmpInst::FIRST_ICMP_PREDICATE && 
         pred <= ICmpInst::LAST_ICMP_PREDICATE && "Invalid ICmp Predicate");

  if (Constant *FC = ConstantFoldCompareInstruction(pred, LHS, RHS))
    return FC;          // Fold a few common cases...

  // Look up the constant in the table first to ensure uniqueness
  std::vector<Constant*> ArgVec;
  ArgVec.push_back(LHS);
  ArgVec.push_back(RHS);
  // Get the key type with both the opcode and predicate
  const ExprMapKeyType Key(Instruction::ICmp, ArgVec, pred);
  return ExprConstants->getOrCreate(Type::Int1Ty, Key);
}

Constant *
ConstantExpr::getFCmp(unsigned short pred, Constant* LHS, Constant* RHS) {
  assert(LHS->getType() == RHS->getType());
  assert(pred <= FCmpInst::LAST_FCMP_PREDICATE && "Invalid FCmp Predicate");

  if (Constant *FC = ConstantFoldCompareInstruction(pred, LHS, RHS))
    return FC;          // Fold a few common cases...

  // Look up the constant in the table first to ensure uniqueness
  std::vector<Constant*> ArgVec;
  ArgVec.push_back(LHS);
  ArgVec.push_back(RHS);
  // Get the key type with both the opcode and predicate
  const ExprMapKeyType Key(Instruction::FCmp, ArgVec, pred);
  return ExprConstants->getOrCreate(Type::Int1Ty, Key);
}

Constant *ConstantExpr::getExtractElementTy(const Type *ReqTy, Constant *Val,
                                            Constant *Idx) {
  if (Constant *FC = ConstantFoldExtractElementInstruction(Val, Idx))
    return FC;          // Fold a few common cases...
  // Look up the constant in the table first to ensure uniqueness
  std::vector<Constant*> ArgVec(1, Val);
  ArgVec.push_back(Idx);
  const ExprMapKeyType Key(Instruction::ExtractElement,ArgVec);
  return ExprConstants->getOrCreate(ReqTy, Key);
}

Constant *ConstantExpr::getExtractElement(Constant *Val, Constant *Idx) {
  assert(isa<VectorType>(Val->getType()) &&
         "Tried to create extractelement operation on non-vector type!");
  assert(Idx->getType() == Type::Int32Ty &&
         "Extractelement index must be i32 type!");
  return getExtractElementTy(cast<VectorType>(Val->getType())->getElementType(),
                             Val, Idx);
}

Constant *ConstantExpr::getInsertElementTy(const Type *ReqTy, Constant *Val,
                                           Constant *Elt, Constant *Idx) {
  if (Constant *FC = ConstantFoldInsertElementInstruction(Val, Elt, Idx))
    return FC;          // Fold a few common cases...
  // Look up the constant in the table first to ensure uniqueness
  std::vector<Constant*> ArgVec(1, Val);
  ArgVec.push_back(Elt);
  ArgVec.push_back(Idx);
  const ExprMapKeyType Key(Instruction::InsertElement,ArgVec);
  return ExprConstants->getOrCreate(ReqTy, Key);
}

Constant *ConstantExpr::getInsertElement(Constant *Val, Constant *Elt, 
                                         Constant *Idx) {
  assert(isa<VectorType>(Val->getType()) &&
         "Tried to create insertelement operation on non-vector type!");
  assert(Elt->getType() == cast<VectorType>(Val->getType())->getElementType()
         && "Insertelement types must match!");
  assert(Idx->getType() == Type::Int32Ty &&
         "Insertelement index must be i32 type!");
  return getInsertElementTy(cast<VectorType>(Val->getType())->getElementType(),
                            Val, Elt, Idx);
}

Constant *ConstantExpr::getShuffleVectorTy(const Type *ReqTy, Constant *V1,
                                           Constant *V2, Constant *Mask) {
  if (Constant *FC = ConstantFoldShuffleVectorInstruction(V1, V2, Mask))
    return FC;          // Fold a few common cases...
  // Look up the constant in the table first to ensure uniqueness
  std::vector<Constant*> ArgVec(1, V1);
  ArgVec.push_back(V2);
  ArgVec.push_back(Mask);
  const ExprMapKeyType Key(Instruction::ShuffleVector,ArgVec);
  return ExprConstants->getOrCreate(ReqTy, Key);
}

Constant *ConstantExpr::getShuffleVector(Constant *V1, Constant *V2, 
                                         Constant *Mask) {
  assert(ShuffleVectorInst::isValidOperands(V1, V2, Mask) &&
         "Invalid shuffle vector constant expr operands!");
  return getShuffleVectorTy(V1->getType(), V1, V2, Mask);
}

Constant *ConstantExpr::getZeroValueForNegationExpr(const Type *Ty) {
  if (const VectorType *PTy = dyn_cast<VectorType>(Ty))
    if (PTy->getElementType()->isFloatingPoint()) {
      std::vector<Constant*> zeros(PTy->getNumElements(),
                           ConstantFP::getNegativeZero(PTy->getElementType()));
      return ConstantVector::get(PTy, zeros);
    }

  if (Ty->isFloatingPoint()) 
    return ConstantFP::getNegativeZero(Ty);

  return Constant::getNullValue(Ty);
}

// destroyConstant - Remove the constant from the constant table...
//
void ConstantExpr::destroyConstant() {
  ExprConstants->remove(this);
  destroyConstantImpl();
}

const char *ConstantExpr::getOpcodeName() const {
  return Instruction::getOpcodeName(getOpcode());
}

//===----------------------------------------------------------------------===//
//                replaceUsesOfWithOnConstant implementations

/// replaceUsesOfWithOnConstant - Update this constant array to change uses of
/// 'From' to be uses of 'To'.  This must update the uniquing data structures
/// etc.
///
/// Note that we intentionally replace all uses of From with To here.  Consider
/// a large array that uses 'From' 1000 times.  By handling this case all here,
/// ConstantArray::replaceUsesOfWithOnConstant is only invoked once, and that
/// single invocation handles all 1000 uses.  Handling them one at a time would
/// work, but would be really slow because it would have to unique each updated
/// array instance.
void ConstantArray::replaceUsesOfWithOnConstant(Value *From, Value *To,
                                                Use *U) {
  assert(isa<Constant>(To) && "Cannot make Constant refer to non-constant!");
  Constant *ToC = cast<Constant>(To);

  std::pair<ArrayConstantsTy::MapKey, Constant*> Lookup;
  Lookup.first.first = getType();
  Lookup.second = this;

  std::vector<Constant*> &Values = Lookup.first.second;
  Values.reserve(getNumOperands());  // Build replacement array.

  // Fill values with the modified operands of the constant array.  Also, 
  // compute whether this turns into an all-zeros array.
  bool isAllZeros = false;
  unsigned NumUpdated = 0;
  if (!ToC->isNullValue()) {
    for (Use *O = OperandList, *E = OperandList+getNumOperands(); O != E; ++O) {
      Constant *Val = cast<Constant>(O->get());
      if (Val == From) {
        Val = ToC;
        ++NumUpdated;
      }
      Values.push_back(Val);
    }
  } else {
    isAllZeros = true;
    for (Use *O = OperandList, *E = OperandList+getNumOperands(); O != E; ++O) {
      Constant *Val = cast<Constant>(O->get());
      if (Val == From) {
        Val = ToC;
        ++NumUpdated;
      }
      Values.push_back(Val);
      if (isAllZeros) isAllZeros = Val->isNullValue();
    }
  }
  
  Constant *Replacement = 0;
  if (isAllZeros) {
    Replacement = ConstantAggregateZero::get(getType());
  } else {
    // Check to see if we have this array type already.
    bool Exists;
    ArrayConstantsTy::MapTy::iterator I =
      ArrayConstants->InsertOrGetItem(Lookup, Exists);
    
    if (Exists) {
      Replacement = I->second;
    } else {
      // Okay, the new shape doesn't exist in the system yet.  Instead of
      // creating a new constant array, inserting it, replaceallusesof'ing the
      // old with the new, then deleting the old... just update the current one
      // in place!
      ArrayConstants->MoveConstantToNewSlot(this, I);
      
      // Update to the new value.  Optimize for the case when we have a single
      // operand that we're changing, but handle bulk updates efficiently.
      if (NumUpdated == 1) {
        unsigned OperandToUpdate = U-OperandList;
        assert(getOperand(OperandToUpdate) == From &&
               "ReplaceAllUsesWith broken!");
        setOperand(OperandToUpdate, ToC);
      } else {
        for (unsigned i = 0, e = getNumOperands(); i != e; ++i)
          if (getOperand(i) == From)
            setOperand(i, ToC);
      }
      return;
    }
  }
 
  // Otherwise, I do need to replace this with an existing value.
  assert(Replacement != this && "I didn't contain From!");
  
  // Everyone using this now uses the replacement.
  uncheckedReplaceAllUsesWith(Replacement);
  
  // Delete the old constant!
  destroyConstant();
}

void ConstantStruct::replaceUsesOfWithOnConstant(Value *From, Value *To,
                                                 Use *U) {
  assert(isa<Constant>(To) && "Cannot make Constant refer to non-constant!");
  Constant *ToC = cast<Constant>(To);

  unsigned OperandToUpdate = U-OperandList;
  assert(getOperand(OperandToUpdate) == From && "ReplaceAllUsesWith broken!");

  std::pair<StructConstantsTy::MapKey, Constant*> Lookup;
  Lookup.first.first = getType();
  Lookup.second = this;
  std::vector<Constant*> &Values = Lookup.first.second;
  Values.reserve(getNumOperands());  // Build replacement struct.
  
  
  // Fill values with the modified operands of the constant struct.  Also, 
  // compute whether this turns into an all-zeros struct.
  bool isAllZeros = false;
  if (!ToC->isNullValue()) {
    for (Use *O = OperandList, *E = OperandList+getNumOperands(); O != E; ++O)
      Values.push_back(cast<Constant>(O->get()));
  } else {
    isAllZeros = true;
    for (Use *O = OperandList, *E = OperandList+getNumOperands(); O != E; ++O) {
      Constant *Val = cast<Constant>(O->get());
      Values.push_back(Val);
      if (isAllZeros) isAllZeros = Val->isNullValue();
    }
  }
  Values[OperandToUpdate] = ToC;
  
  Constant *Replacement = 0;
  if (isAllZeros) {
    Replacement = ConstantAggregateZero::get(getType());
  } else {
    // Check to see if we have this array type already.
    bool Exists;
    StructConstantsTy::MapTy::iterator I =
      StructConstants->InsertOrGetItem(Lookup, Exists);
    
    if (Exists) {
      Replacement = I->second;
    } else {
      // Okay, the new shape doesn't exist in the system yet.  Instead of
      // creating a new constant struct, inserting it, replaceallusesof'ing the
      // old with the new, then deleting the old... just update the current one
      // in place!
      StructConstants->MoveConstantToNewSlot(this, I);
      
      // Update to the new value.
      setOperand(OperandToUpdate, ToC);
      return;
    }
  }
  
  assert(Replacement != this && "I didn't contain From!");
  
  // Everyone using this now uses the replacement.
  uncheckedReplaceAllUsesWith(Replacement);
  
  // Delete the old constant!
  destroyConstant();
}

void ConstantVector::replaceUsesOfWithOnConstant(Value *From, Value *To,
                                                 Use *U) {
  assert(isa<Constant>(To) && "Cannot make Constant refer to non-constant!");
  
  std::vector<Constant*> Values;
  Values.reserve(getNumOperands());  // Build replacement array...
  for (unsigned i = 0, e = getNumOperands(); i != e; ++i) {
    Constant *Val = getOperand(i);
    if (Val == From) Val = cast<Constant>(To);
    Values.push_back(Val);
  }
  
  Constant *Replacement = ConstantVector::get(getType(), Values);
  assert(Replacement != this && "I didn't contain From!");
  
  // Everyone using this now uses the replacement.
  uncheckedReplaceAllUsesWith(Replacement);
  
  // Delete the old constant!
  destroyConstant();
}

void ConstantExpr::replaceUsesOfWithOnConstant(Value *From, Value *ToV,
                                               Use *U) {
  assert(isa<Constant>(ToV) && "Cannot make Constant refer to non-constant!");
  Constant *To = cast<Constant>(ToV);
  
  Constant *Replacement = 0;
  if (getOpcode() == Instruction::GetElementPtr) {
    SmallVector<Constant*, 8> Indices;
    Constant *Pointer = getOperand(0);
    Indices.reserve(getNumOperands()-1);
    if (Pointer == From) Pointer = To;
    
    for (unsigned i = 1, e = getNumOperands(); i != e; ++i) {
      Constant *Val = getOperand(i);
      if (Val == From) Val = To;
      Indices.push_back(Val);
    }
    Replacement = ConstantExpr::getGetElementPtr(Pointer,
                                                 &Indices[0], Indices.size());
  } else if (isCast()) {
    assert(getOperand(0) == From && "Cast only has one use!");
    Replacement = ConstantExpr::getCast(getOpcode(), To, getType());
  } else if (getOpcode() == Instruction::Select) {
    Constant *C1 = getOperand(0);
    Constant *C2 = getOperand(1);
    Constant *C3 = getOperand(2);
    if (C1 == From) C1 = To;
    if (C2 == From) C2 = To;
    if (C3 == From) C3 = To;
    Replacement = ConstantExpr::getSelect(C1, C2, C3);
  } else if (getOpcode() == Instruction::ExtractElement) {
    Constant *C1 = getOperand(0);
    Constant *C2 = getOperand(1);
    if (C1 == From) C1 = To;
    if (C2 == From) C2 = To;
    Replacement = ConstantExpr::getExtractElement(C1, C2);
  } else if (getOpcode() == Instruction::InsertElement) {
    Constant *C1 = getOperand(0);
    Constant *C2 = getOperand(1);
    Constant *C3 = getOperand(1);
    if (C1 == From) C1 = To;
    if (C2 == From) C2 = To;
    if (C3 == From) C3 = To;
    Replacement = ConstantExpr::getInsertElement(C1, C2, C3);
  } else if (getOpcode() == Instruction::ShuffleVector) {
    Constant *C1 = getOperand(0);
    Constant *C2 = getOperand(1);
    Constant *C3 = getOperand(2);
    if (C1 == From) C1 = To;
    if (C2 == From) C2 = To;
    if (C3 == From) C3 = To;
    Replacement = ConstantExpr::getShuffleVector(C1, C2, C3);
  } else if (isCompare()) {
    Constant *C1 = getOperand(0);
    Constant *C2 = getOperand(1);
    if (C1 == From) C1 = To;
    if (C2 == From) C2 = To;
    if (getOpcode() == Instruction::ICmp)
      Replacement = ConstantExpr::getICmp(getPredicate(), C1, C2);
    else
      Replacement = ConstantExpr::getFCmp(getPredicate(), C1, C2);
  } else if (getNumOperands() == 2) {
    Constant *C1 = getOperand(0);
    Constant *C2 = getOperand(1);
    if (C1 == From) C1 = To;
    if (C2 == From) C2 = To;
    Replacement = ConstantExpr::get(getOpcode(), C1, C2);
  } else {
    assert(0 && "Unknown ConstantExpr type!");
    return;
  }
  
  assert(Replacement != this && "I didn't contain From!");
  
  // Everyone using this now uses the replacement.
  uncheckedReplaceAllUsesWith(Replacement);
  
  // Delete the old constant!
  destroyConstant();
}


/// getStringValue - Turn an LLVM constant pointer that eventually points to a
/// global into a string value.  Return an empty string if we can't do it.
/// Parameter Chop determines if the result is chopped at the first null
/// terminator.
///
std::string Constant::getStringValue(bool Chop, unsigned Offset) {
  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(this)) {
    if (GV->hasInitializer() && isa<ConstantArray>(GV->getInitializer())) {
      ConstantArray *Init = cast<ConstantArray>(GV->getInitializer());
      if (Init->isString()) {
        std::string Result = Init->getAsString();
        if (Offset < Result.size()) {
          // If we are pointing INTO The string, erase the beginning...
          Result.erase(Result.begin(), Result.begin()+Offset);

          // Take off the null terminator, and any string fragments after it.
          if (Chop) {
            std::string::size_type NullPos = Result.find_first_of((char)0);
            if (NullPos != std::string::npos)
              Result.erase(Result.begin()+NullPos, Result.end());
          }
          return Result;
        }
      }
    }
  } else if (Constant *C = dyn_cast<Constant>(this)) {
    if (GlobalValue *GV = dyn_cast<GlobalValue>(C))
      return GV->getStringValue(Chop, Offset);
    else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(C)) {
      if (CE->getOpcode() == Instruction::GetElementPtr) {
        // Turn a gep into the specified offset.
        if (CE->getNumOperands() == 3 &&
            cast<Constant>(CE->getOperand(1))->isNullValue() &&
            isa<ConstantInt>(CE->getOperand(2))) {
          Offset += cast<ConstantInt>(CE->getOperand(2))->getZExtValue();
          return CE->getOperand(0)->getStringValue(Chop, Offset);
        }
      }
    }
  }
  return "";
}
