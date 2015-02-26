/*
 * Copyright © 2012 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 */

// Copyright (c) 2003-2014 University of Illinois at Urbana-Champaign.
// All rights reserved.
//
// Developed by:
//
//    LLVM Team
//
//    University of Illinois at Urbana-Champaign
//
//    http://llvm.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal with
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
//
//    * Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimers.
//
//   * Redistributions in binary form must reproduce the above copyright notice,
//      this list of conditions and the following disclaimers in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the names of the LLVM Team, University of Illinois at
//      Urbana-Champaign, nor the names of its contributors may be used to
//      endorse or promote products derived from this Software without specific
//      prior written permission.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE
// SOFTWARE.

//===- ExpandLargeIntegers.cpp - Expand illegal integers for PNaCl ABI ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License.
//
// A limited set of transformations to expand illegal-sized int types.
//
//===----------------------------------------------------------------------===//
//
// Legal sizes for the purposes of expansion are anything 64 bits or less.
// Operations on large integers are split into operations on smaller-sized
// integers. The low parts should always be powers of 2, but the high parts may
// not be. A subsequent pass can promote those. For now this pass only intends
// to support the uses generated by clang, which is basically just for large
// bitfields.
//
// Limitations:
// 1) It can't change function signatures or global variables.
// 3) Doesn't support mul, div/rem, switch.
// 4) Doesn't handle arrays or structs (or GEPs) with illegal types.
// 5) Doesn't handle constant expressions (it also doesn't produce them, so it
//    can run after ExpandConstantExpr).
//
// The PNaCl version does not handle bitcast between vector and large integer.
// So I develop the bitcast from/to vector logic.
// TODO: 1. When we do lshr/trunc, and we know it is cast from a vector, we can
//          optimize it to extractElement.
//       2. OR x, 0 can be optimized as x. And x, 0 can be optimized as 0.
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#if LLVM_VERSION_MINOR >= 5
#include "llvm/IR/CFG.h"
#else
#include "llvm/Support/CFG.h"
#endif
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm_gen_backend.hpp"

using namespace llvm;

#if LLVM_VERSION_MINOR >= 5
#define DEBUG_TYPE "nacl-expand-ints"
#endif

#ifdef DEBUG
  #undef DEBUG
  #define DEBUG(...)
#endif
// Break instructions up into no larger than 64-bit chunks.
static const unsigned kChunkBits = 64;
static const unsigned kChunkBytes = kChunkBits / CHAR_BIT;

namespace {
class ExpandLargeIntegers : public FunctionPass {
public:
  static char ID;
  ExpandLargeIntegers() : FunctionPass(ID) {
  }
  bool runOnFunction(Function &F) override;
};

template <typename T> struct LoHiPair {
  T Lo, Hi;
  LoHiPair() : Lo(), Hi() {}
  LoHiPair(T Lo, T Hi) : Lo(Lo), Hi(Hi) {}
};
typedef LoHiPair<IntegerType *> TypePair;
typedef LoHiPair<Value *> ValuePair;
typedef LoHiPair<unsigned> AlignPair;

struct VectorElement {
  Value *parent;
  unsigned childId;
  VectorElement() : parent(NULL), childId(0) {}
  VectorElement(Value *p, unsigned i) : parent(p), childId(i) {}
};

// Information needed to patch a phi node which forward-references a value.
struct ForwardPHI {
  Value *Val;
  PHINode *Lo, *Hi;
  unsigned ValueNumber;
  ForwardPHI(Value *Val, PHINode *Lo, PHINode *Hi, unsigned ValueNumber)
      : Val(Val), Lo(Lo), Hi(Hi), ValueNumber(ValueNumber) {}
};
}

char ExpandLargeIntegers::ID = 0;

static bool isLegalBitSize(unsigned Bits) {
  assert(Bits && "Can't have zero-size integers");
  return Bits <= kChunkBits;
}

static TypePair getExpandedIntTypes(Type *Ty) {
  unsigned BitWidth = Ty->getIntegerBitWidth();
  assert(!isLegalBitSize(BitWidth));
  return TypePair(IntegerType::get(Ty->getContext(), kChunkBits),
                  IntegerType::get(Ty->getContext(), BitWidth - kChunkBits));
}

// Return true if Val is an int which should be converted.
static bool shouldConvert(const Value *Val) {
  Type *Ty = Val->getType();
  if (IntegerType *ITy = dyn_cast<IntegerType>(Ty))
    return !isLegalBitSize(ITy->getBitWidth());
  return false;
}

// Return a pair of constants expanded from C.
static ValuePair expandConstant(Constant *C) {
  assert(shouldConvert(C));
  TypePair ExpandedTypes = getExpandedIntTypes(C->getType());
  if (isa<UndefValue>(C)) {
    return ValuePair(UndefValue::get(ExpandedTypes.Lo),
                     UndefValue::get(ExpandedTypes.Hi));
  } else if (ConstantInt *CInt = dyn_cast<ConstantInt>(C)) {
    Constant *ShiftAmt = ConstantInt::get(
        CInt->getType(), ExpandedTypes.Lo->getBitWidth(), false);
    return ValuePair(
        ConstantExpr::getTrunc(CInt, ExpandedTypes.Lo),
        ConstantExpr::getTrunc(ConstantExpr::getLShr(CInt, ShiftAmt),
                               ExpandedTypes.Hi));
  }
  errs() << "Value: " << *C << "\n";
  report_fatal_error("Unexpected constant value");
}

template <typename T>
static AlignPair getAlign(const DataLayout &DL, T *I, Type *PrefAlignTy) {
  unsigned LoAlign = I->getAlignment();
  if (LoAlign == 0)
    LoAlign = DL.getPrefTypeAlignment(PrefAlignTy);
  unsigned HiAlign = MinAlign(LoAlign, kChunkBytes);
  return AlignPair(LoAlign, HiAlign);
}

namespace {
// Holds the state for converting/replacing values. We visit instructions in
// reverse post-order, phis are therefore the only instructions which can be
// visited before the value they use.
class ConversionState {
public:
  // Return the expanded values for Val.
  ValuePair getConverted(Value *Val) {
    assert(shouldConvert(Val));
    // Directly convert constants.
    if (Constant *C = dyn_cast<Constant>(Val))
      return expandConstant(C);
    if (RewrittenIllegals.count(Val)) {
      ValuePair Found = RewrittenIllegals[Val];
      if (RewrittenLegals.count(Found.Lo))
        Found.Lo = RewrittenLegals[Found.Lo];
      if (RewrittenLegals.count(Found.Hi))
        Found.Hi = RewrittenLegals[Found.Hi];
      return Found;
    }
    errs() << "Value: " << *Val << "\n";
    report_fatal_error("Expanded value not found in map");
  }

  // Returns whether a converted value has been recorded. This is only useful
  // for phi instructions: they can be encountered before the incoming
  // instruction, whereas RPO order guarantees that other instructions always
  // use converted values.
  bool hasConverted(Value *Val) {
    assert(shouldConvert(Val));
    return dyn_cast<Constant>(Val) || RewrittenIllegals.count(Val);
  }

  // Record a forward phi, temporarily setting it to use Undef. This will be
  // patched up at the end of RPO.
  ValuePair recordForwardPHI(Value *Val, PHINode *Lo, PHINode *Hi,
                             unsigned ValueNumber) {
    DEBUG(dbgs() << "\tRecording as forward PHI\n");
    ForwardPHIs.push_back(ForwardPHI(Val, Lo, Hi, ValueNumber));
    return ValuePair(UndefValue::get(Lo->getType()),
                     UndefValue::get(Hi->getType()));
  }

  void recordConverted(Instruction *From, const ValuePair &To) {
    DEBUG(dbgs() << "\tTo:  " << *To.Lo << "\n");
    DEBUG(dbgs() << "\tAnd: " << *To.Hi << "\n");
    ToErase.push_back(From);
    RewrittenIllegals[From] = To;
  }

  // Replace the uses of From with To, give From's name to To, and mark To for
  // deletion.
  void recordConverted(Instruction *From, Value *To) {
    assert(!shouldConvert(From));
    DEBUG(dbgs() << "\tTo:  " << *To << "\n");
    ToErase.push_back(From);
    // From does not produce an illegal value, update its users in place.
    From->replaceAllUsesWith(To);
    To->takeName(From);
    RewrittenLegals[From] = To;
  }

  void patchForwardPHIs() {
    DEBUG(if (!ForwardPHIs.empty()) dbgs() << "Patching forward PHIs:\n");
    for (ForwardPHI &F : ForwardPHIs) {
      ValuePair Ops = getConverted(F.Val);
      F.Lo->setIncomingValue(F.ValueNumber, Ops.Lo);
      F.Hi->setIncomingValue(F.ValueNumber, Ops.Hi);
      DEBUG(dbgs() << "\t" << *F.Lo << "\n\t" << *F.Hi << "\n");
    }
  }

  void eraseReplacedInstructions() {
    for (Instruction *I : ToErase)
      I->dropAllReferences();

    for (Instruction *I : ToErase)
      I->eraseFromParent();
  }

  void addEraseCandidate(Instruction *c) {
    ToErase.push_back(c);
  }

  void appendElement(Value *v, Value *e) {
    if (ExtractElement.count(v) == 0) {
      SmallVector<Value *, 16> tmp;
      tmp.push_back(e);
      ExtractElement[v] = tmp;
    } else
      ExtractElement[v].push_back(e);
  }

  Value *getElement(Value *v, unsigned id) {
    return (ExtractElement[v])[id];
  }
  VectorElement &getVectorMap(Value *child) {
    return VectorIllegals[child];
  }

  bool convertedVector(Value *vector) {
    return VectorIllegals.count(vector) > 0 ? true : false;
  }

  void recordVectorMap(Value *child, VectorElement elem) {
    VectorIllegals[child] = elem;
  }

private:
  // Maps illegal values to their new converted lo/hi values.
  DenseMap<Value *, ValuePair> RewrittenIllegals;
  // Maps legal values to their new converted value.
  DenseMap<Value *, Value *> RewrittenLegals;
  // Illegal values which have already been converted, will be erased.
  SmallVector<Instruction *, 32> ToErase;
  // PHIs which were encountered but had forward references. They need to get
  // patched up after RPO traversal.
  SmallVector<ForwardPHI, 32> ForwardPHIs;
  // helpers to solve bitcasting from vector to illegal integer types
  // Maps a Value to its original Vector and elemId
  DenseMap<Value *, VectorElement> VectorIllegals;
  // cache the ExtractElement Values
  DenseMap<Value *, SmallVector<Value *, 16>> ExtractElement;
};
} // Anonymous namespace

static Value *buildVectorOrScalar(ConversionState &State, IRBuilder<> &IRB, SmallVector<Value *, 16> Elements) {
  assert(!Elements.empty());
  Type *IntTy = IntegerType::get(IRB.getContext(), 32);

  if (Elements.size() > 1) {
    Value * vec = NULL;
    unsigned ElemNo = Elements.size();
    Type *ElemTy = Elements[0]->getType();
    bool KeepInsert = isLegalBitSize(ElemTy->getPrimitiveSizeInBits() * ElemNo);
    for (unsigned i = 0; i < ElemNo; ++i) {
      Value *tmp = vec ? vec : UndefValue::get(VectorType::get(ElemTy, ElemNo));
      Value *idx = ConstantInt::get(IntTy, i);
      vec = IRB.CreateInsertElement(tmp, Elements[i], idx);
      if (!KeepInsert) {
        State.addEraseCandidate(cast<Instruction>(vec));
      }
    }
    return vec;
  } else {
    return Elements[0];
  }
}

static void getSplitedValue(ConversionState &State, Value *Val, SmallVector<Value *, 16> &Result) {
  while (shouldConvert(Val)) {
    ValuePair Convert = State.getConverted(Val);
    Result.push_back(Convert.Lo);
    Val = Convert.Hi;
  }
  Result.push_back(Val);
}

// make all the elements in Src use the same llvm::Type, and return them in Dst
static void unifyElementType(IRBuilder<> &IRB, SmallVector<Value *, 16> &Src, SmallVector<Value *, 16> &Dst) {
  unsigned MinWidth = Src[0]->getType()->getPrimitiveSizeInBits();
  bool Unified = true;
  for (unsigned i = 0; i < Src.size(); i++) {
    Type *Ty = Src[i]->getType();
    unsigned BitWidth = Ty->getPrimitiveSizeInBits();
    if(BitWidth != MinWidth) Unified = false;
    if(BitWidth < MinWidth) MinWidth = BitWidth;
  }

  if (Unified) {
    for (unsigned i = 0; i < Src.size(); i++)
      Dst.push_back(Src[i]);
  } else {
    Type *IntTy = IntegerType::get(IRB.getContext(), 32);
    Type *ElemTy = IntegerType::get(IRB.getContext(), MinWidth);
    for (unsigned i = 0; i < Src.size(); i++) {
      Type *Ty = Src[i]->getType();
      unsigned Size = Ty->getPrimitiveSizeInBits();
      assert((Size % MinWidth) == 0);

      if (Size > MinWidth) {
        VectorType *VecTy = VectorType::get(ElemTy, Size/MinWidth);
        Value *Casted = IRB.CreateBitCast(Src[i], VecTy);
        for (unsigned j = 0; j < Size/MinWidth; j++)
          Dst.push_back(IRB.CreateExtractElement(Casted, ConstantInt::get(IntTy, j)));
      } else {
        Dst.push_back(Src[i]);
      }
    }
  }
}

static void convertInstruction(Instruction *Inst, ConversionState &State,
                               const DataLayout &DL) {
  DEBUG(dbgs() << "Expanding Large Integer: " << *Inst << "\n");
  // Set the insert point *after* Inst, so that any instructions inserted here
  // will be visited again. That allows iterative expansion of types > i128.
  BasicBlock::iterator InsertPos(Inst);
  IRBuilder<> IRB(++InsertPos);
  StringRef Name = Inst->getName();

  if (PHINode *Phi = dyn_cast<PHINode>(Inst)) {
    unsigned N = Phi->getNumIncomingValues();
    TypePair OpTys = getExpandedIntTypes(Phi->getIncomingValue(0)->getType());
    PHINode *Lo = IRB.CreatePHI(OpTys.Lo, N, Twine(Name + ".lo"));
    PHINode *Hi = IRB.CreatePHI(OpTys.Hi, N, Twine(Name + ".hi"));
    for (unsigned I = 0; I != N; ++I) {
      Value *InVal = Phi->getIncomingValue(I);
      BasicBlock *InBB = Phi->getIncomingBlock(I);
      // If the value hasn't already been converted then this is a
      // forward-reference PHI which needs to be patched up after RPO traversal.
      ValuePair Ops = State.hasConverted(InVal)
                          ? State.getConverted(InVal)
                          : State.recordForwardPHI(InVal, Lo, Hi, I);
      Lo->addIncoming(Ops.Lo, InBB);
      Hi->addIncoming(Ops.Hi, InBB);
    }
    State.recordConverted(Phi, ValuePair(Lo, Hi));

  } else if (ZExtInst *ZExt = dyn_cast<ZExtInst>(Inst)) {
    Value *Operand = ZExt->getOperand(0);
    Type *OpTy = Operand->getType();
    TypePair Tys = getExpandedIntTypes(Inst->getType());
    Value *Lo, *Hi;
    if (OpTy->getIntegerBitWidth() <= kChunkBits) {
      Lo = IRB.CreateZExt(Operand, Tys.Lo, Twine(Name, ".lo"));
      Hi = ConstantInt::get(Tys.Hi, 0);
    } else {
      ValuePair Ops = State.getConverted(Operand);
      Lo = Ops.Lo;
      Hi = IRB.CreateZExt(Ops.Hi, Tys.Hi, Twine(Name, ".hi"));
    }
    State.recordConverted(ZExt, ValuePair(Lo, Hi));

  } else if (TruncInst *Trunc = dyn_cast<TruncInst>(Inst)) {
    Value *Operand = Trunc->getOperand(0);
    assert(shouldConvert(Operand) && "TruncInst is expandable but not its op");
    TypePair OpTys = getExpandedIntTypes(Operand->getType());
    ValuePair Ops = State.getConverted(Operand);
    if (!shouldConvert(Inst)) {
      Value *NewInst = IRB.CreateTrunc(Ops.Lo, Trunc->getType(), Name);
      State.recordConverted(Trunc, NewInst);
    } else {
      TypePair Tys = getExpandedIntTypes(Trunc->getType());
      assert(Tys.Lo == OpTys.Lo);
      Value *Lo = Ops.Lo;
      Value *Hi = IRB.CreateTrunc(Ops.Hi, Tys.Hi, Twine(Name, ".hi"));
      State.recordConverted(Trunc, ValuePair(Lo, Hi));
    }

  } else if (BitCastInst *Cast = dyn_cast<BitCastInst>(Inst)) {
    Value *Operand = Cast->getOperand(0);
    bool DstVec = Inst->getType()->isVectorTy();

    Type *IntTy = IntegerType::get(Cast->getContext(), 32);
    if (DstVec) {
      // integer to vector, get all children and bitcast
      SmallVector<Value *, 16> Split;
      SmallVector<Value *, 16> Unified;
      getSplitedValue(State, Operand, Split);
      // unify element type, this is required by insertelement
      unifyElementType(IRB, Split, Unified);

      Value *vec = NULL;
      unsigned ElemNo = Unified.size();
      Type *ElemTy = Unified[0]->getType();
      for (unsigned i = 0; i < ElemNo; ++i) {
        Value *tmp = vec ? vec : UndefValue::get(VectorType::get(ElemTy, ElemNo));
        Value *idx = ConstantInt::get(IntTy, i);
        vec = IRB.CreateInsertElement(tmp, Unified[i], idx);
      }
      if (vec->getType() != Cast->getType())
        vec = IRB.CreateBitCast(vec, Cast->getType());
      State.recordConverted(Cast, vec);
    } else {
      // vector to integer
      assert(Operand->getType()->isVectorTy());
      VectorType *VecTy = cast<VectorType>(Operand->getType());
      Type *LargeTy = Inst->getType();
      Type *ElemTy = VecTy->getElementType();
      unsigned ElemNo = VecTy->getNumElements();
      Value * VectorRoot = NULL;
      unsigned ChildIndex = 0;

      if (State.convertedVector(Operand)) {
        VectorElement VE = State.getVectorMap(Operand);
        VectorRoot = VE.parent;
        ChildIndex = VE.childId;
      } else {
        for (unsigned i =0; i < ElemNo; i++)
          State.appendElement(Operand,
                              IRB.CreateExtractElement(Operand, ConstantInt::get(IntTy, i))
                             );
        VectorRoot = Operand;
      }

      TypePair OpTys = getExpandedIntTypes(LargeTy);
      Value *Lo, *Hi;
      unsigned LowNo = OpTys.Lo->getIntegerBitWidth() / ElemTy->getPrimitiveSizeInBits();
      unsigned HighNo = OpTys.Hi->getIntegerBitWidth() / ElemTy->getPrimitiveSizeInBits();

      SmallVector<Value *, 16> LoElems;
      for (unsigned i = 0; i < LowNo; ++i)
        LoElems.push_back(State.getElement(VectorRoot, i+ChildIndex));

      Lo = IRB.CreateBitCast(buildVectorOrScalar(State, IRB, LoElems), OpTys.Lo, Twine(Name, ".lo"));

      SmallVector<Value *, 16> HiElem;
      for (unsigned i = 0; i < HighNo; ++i)
        HiElem.push_back(State.getElement(VectorRoot, i+LowNo+ChildIndex));

      Value *NewVec = buildVectorOrScalar(State, IRB, HiElem);
      Hi = IRB.CreateBitCast(NewVec, OpTys.Hi);

      State.recordVectorMap(NewVec, VectorElement(VectorRoot, LowNo + ChildIndex));
      State.recordConverted(Cast, ValuePair(Lo, Hi));
    }

  } else if (BinaryOperator *Binop = dyn_cast<BinaryOperator>(Inst)) {
    ValuePair Lhs = State.getConverted(Binop->getOperand(0));
    ValuePair Rhs = State.getConverted(Binop->getOperand(1));
    TypePair Tys = getExpandedIntTypes(Binop->getType());
    Instruction::BinaryOps Op = Binop->getOpcode();
    switch (Op) {
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor: {
      Value *Lo = IRB.CreateBinOp(Op, Lhs.Lo, Rhs.Lo, Twine(Name, ".lo"));
      Value *Hi = IRB.CreateBinOp(Op, Lhs.Hi, Rhs.Hi, Twine(Name, ".hi"));
      State.recordConverted(Binop, ValuePair(Lo, Hi));
      break;
    }

    case Instruction::Shl: {
      ConstantInt *ShlAmount = dyn_cast<ConstantInt>(Rhs.Lo);
      // TODO(dschuff): Expansion of variable-sized shifts isn't supported
      // because the behavior depends on whether the shift amount is less than
      // the size of the low part of the expanded type, and I haven't yet
      // figured out a way to do it for variable-sized shifts without splitting
      // the basic block. I don't believe it's actually necessary for
      // bitfields. Likewise for LShr below.
      if (!ShlAmount) {
        errs() << "Shift: " << *Binop << "\n";
        report_fatal_error("Expansion of variable-sized shifts of > 64-bit-"
                           "wide values is not supported");
      }
      unsigned ShiftAmount = ShlAmount->getZExtValue();
      if (ShiftAmount >= Binop->getType()->getIntegerBitWidth())
        ShiftAmount = 0; // Undefined behavior.

      unsigned HiBits = Tys.Hi->getIntegerBitWidth();
      // |<------------Hi---------->|<-------Lo------>|
      // |                          |                 |
      // +--------+--------+--------+--------+--------+
      // |abcdefghijklmnopqrstuvwxyz|ABCDEFGHIJKLMNOPQ|
      // +--------+--------+--------+--------+--------+
      // Possible shifts:
      // |efghijklmnopqrstuvwxyzABCD|EFGHIJKLMNOPQ0000| Some Lo into Hi.
      // |vwxyzABCDEFGHIJKLMNOPQ0000|00000000000000000| Lo is 0, keep some Hi.
      // |DEFGHIJKLMNOPQ000000000000|00000000000000000| Lo is 0, no Hi left.
      Value *Lo, *Hi;
      if (ShiftAmount < kChunkBits) {
        Lo = IRB.CreateShl(Lhs.Lo, ShiftAmount, Twine(Name, ".lo"));
        Hi = IRB.CreateZExtOrTrunc(IRB.CreateLShr(Lhs.Lo,
                                                  kChunkBits - ShiftAmount,
                                                  Twine(Name, ".lo.shr")),
                                   Tys.Hi, Twine(Name, ".lo.ext"));
      } else {
        Lo = ConstantInt::get(Tys.Lo, 0);
        if (ShiftAmount == kChunkBits) {
          // Hi will be from Lo
          Hi = IRB.CreateZExtOrTrunc(Lhs.Lo, Tys.Hi, Twine(Name, ".lo.ext"));
        } else {
          Hi = IRB.CreateShl(
              IRB.CreateZExtOrTrunc(Lhs.Lo, Tys.Hi, Twine(Name, ".lo.ext")),
              ShiftAmount - kChunkBits, Twine(Name, ".lo.shl"));
        }
      }
      if (ShiftAmount < HiBits)
        Hi = IRB.CreateOr(
            Hi, IRB.CreateShl(Lhs.Hi, ShiftAmount, Twine(Name, ".hi.shl")),
            Twine(Name, ".or"));
      State.recordConverted(Binop, ValuePair(Lo, Hi));
      break;
    }

    case Instruction::AShr:
    case Instruction::LShr: {
      ConstantInt *ShrAmount = dyn_cast<ConstantInt>(Rhs.Lo);
      // TODO(dschuff): Expansion of variable-sized shifts isn't supported
      // because the behavior depends on whether the shift amount is less than
      // the size of the low part of the expanded type, and I haven't yet
      // figured out a way to do it for variable-sized shifts without splitting
      // the basic block. I don't believe it's actually necessary for bitfields.
      if (!ShrAmount) {
        errs() << "Shift: " << *Binop << "\n";
        report_fatal_error("Expansion of variable-sized shifts of > 64-bit-"
                           "wide values is not supported");
      }
      bool IsArith = Op == Instruction::AShr;
      unsigned ShiftAmount = ShrAmount->getZExtValue();
      if (ShiftAmount >= Binop->getType()->getIntegerBitWidth())
        ShiftAmount = 0; // Undefined behavior.
      unsigned HiBitWidth = Tys.Hi->getIntegerBitWidth();
      // |<--Hi-->|<-------Lo------>|
      // |        |                 |
      // +--------+--------+--------+
      // |abcdefgh|ABCDEFGHIJKLMNOPQ|
      // +--------+--------+--------+
      // Possible shifts (0 is sign when doing AShr):
      // |0000abcd|defgABCDEFGHIJKLM| Some Hi into Lo.
      // |00000000|00abcdefgABCDEFGH| Hi is 0, keep some Lo.
      // |00000000|000000000000abcde| Hi is 0, no Lo left.
      Value *Lo, *Hi;
      if (ShiftAmount == 0) {
        Lo = Lhs.Lo; Hi = Lhs.Hi;
      } else {
        if (ShiftAmount < kChunkBits) {
          Lo = IRB.CreateShl(
              IsArith
                  ? IRB.CreateSExtOrTrunc(Lhs.Hi, Tys.Lo, Twine(Name, ".hi.ext"))
                  : IRB.CreateZExtOrTrunc(Lhs.Hi, Tys.Lo, Twine(Name, ".hi.ext")),
              kChunkBits - ShiftAmount, Twine(Name, ".hi.shl"));

          Lo = IRB.CreateOr(
              Lo, IRB.CreateLShr(Lhs.Lo, ShiftAmount, Twine(Name, ".lo.shr")),
              Twine(Name, ".lo"));
        } else if (ShiftAmount == kChunkBits) {
          Lo = IsArith
                  ? IRB.CreateSExtOrTrunc(Lhs.Hi, Tys.Lo, Twine(Name, ".hi.ext"))
                  : IRB.CreateZExtOrTrunc(Lhs.Hi, Tys.Lo, Twine(Name, ".hi.ext"));

        } else {
          Lo = IRB.CreateBinOp(Op, Lhs.Hi,
                               ConstantInt::get(Tys.Hi, ShiftAmount - kChunkBits),
                               Twine(Name, ".hi.shr"));
          Lo = IsArith
                   ? IRB.CreateSExtOrTrunc(Lo, Tys.Lo, Twine(Name, ".lo.ext"))
                   : IRB.CreateZExtOrTrunc(Lo, Tys.Lo, Twine(Name, ".lo.ext"));
        }
        if (ShiftAmount < HiBitWidth) {
          Hi = IRB.CreateBinOp(Op, Lhs.Hi, ConstantInt::get(Tys.Hi, ShiftAmount),
                               Twine(Name, ".hi"));
        } else {
          Hi = IsArith
                   ? IRB.CreateAShr(Lhs.Hi, HiBitWidth - 1, Twine(Name, ".hi"))
                   : ConstantInt::get(Tys.Hi, 0);
        }
      }
      State.recordConverted(Binop, ValuePair(Lo, Hi));
      break;
    }

    case Instruction::Add:
    case Instruction::Sub: {
      Value *Lo, *Hi;
      if (Op == Instruction::Add) {
        Value *Limit = IRB.CreateSelect(
            IRB.CreateICmpULT(Lhs.Lo, Rhs.Lo, Twine(Name, ".cmp")), Rhs.Lo,
            Lhs.Lo, Twine(Name, ".limit"));
        // Don't propagate NUW/NSW to the lo operation: it can overflow.
        Lo = IRB.CreateBinOp(Op, Lhs.Lo, Rhs.Lo, Twine(Name, ".lo"));
        Value *Carry = IRB.CreateZExt(
            IRB.CreateICmpULT(Lo, Limit, Twine(Name, ".overflowed")), Tys.Hi,
            Twine(Name, ".carry"));
        // TODO(jfb) The hi operation could be tagged with NUW/NSW.
        Hi = IRB.CreateBinOp(
            Op, IRB.CreateBinOp(Op, Lhs.Hi, Rhs.Hi, Twine(Name, ".hi")), Carry,
            Twine(Name, ".carried"));
      } else {
        Value *Borrowed = IRB.CreateSExt(
            IRB.CreateICmpULT(Lhs.Lo, Rhs.Lo, Twine(Name, ".borrow")), Tys.Hi,
            Twine(Name, ".borrowing"));
        Lo = IRB.CreateBinOp(Op, Lhs.Lo, Rhs.Lo, Twine(Name, ".lo"));
        Hi = IRB.CreateBinOp(
            Instruction::Add,
            IRB.CreateBinOp(Op, Lhs.Hi, Rhs.Hi, Twine(Name, ".hi")), Borrowed,
            Twine(Name, ".borrowed"));
      }
      State.recordConverted(Binop, ValuePair(Lo, Hi));
      break;
    }

    default:
      errs() << "Operation: " << *Binop << "\n";
      report_fatal_error("Unhandled BinaryOperator type in "
                         "ExpandLargeIntegers");
    }

  } else if (LoadInst *Load = dyn_cast<LoadInst>(Inst)) {
    Value *Op = Load->getPointerOperand();
    unsigned AddrSpace = Op->getType()->getPointerAddressSpace();
    TypePair Tys = getExpandedIntTypes(Load->getType());
    AlignPair Align = getAlign(DL, Load, Load->getType());
    Value *Loty = IRB.CreateBitCast(Op, Tys.Lo->getPointerTo(AddrSpace),
                                    Twine(Op->getName(), ".loty"));
    Value *Lo =
        IRB.CreateAlignedLoad(Loty, Align.Lo, Twine(Load->getName(), ".lo"));
    Value *HiAddr =
        IRB.CreateConstGEP1_32(Loty, 1, Twine(Op->getName(), ".hi.gep"));
    Value *HiTy = IRB.CreateBitCast(HiAddr, Tys.Hi->getPointerTo(AddrSpace),
                                    Twine(Op->getName(), ".hity"));
    Value *Hi =
        IRB.CreateAlignedLoad(HiTy, Align.Hi, Twine(Load->getName(), ".hi"));
    State.recordConverted(Load, ValuePair(Lo, Hi));

  } else if (StoreInst *Store = dyn_cast<StoreInst>(Inst)) {
    Value *Ptr = Store->getPointerOperand();
    unsigned AddrSpace = Ptr->getType()->getPointerAddressSpace();
    TypePair Tys = getExpandedIntTypes(Store->getValueOperand()->getType());
    ValuePair StoreVals = State.getConverted(Store->getValueOperand());
    AlignPair Align = getAlign(DL, Store, Store->getValueOperand()->getType());
    Value *Loty = IRB.CreateBitCast(Ptr, Tys.Lo->getPointerTo(AddrSpace),
                                    Twine(Ptr->getName(), ".loty"));
    Value *Lo = IRB.CreateAlignedStore(StoreVals.Lo, Loty, Align.Lo);
    Value *HiAddr =
        IRB.CreateConstGEP1_32(Loty, 1, Twine(Ptr->getName(), ".hi.gep"));
    Value *HiTy = IRB.CreateBitCast(HiAddr, Tys.Hi->getPointerTo(AddrSpace),
                                    Twine(Ptr->getName(), ".hity"));
    Value *Hi = IRB.CreateAlignedStore(StoreVals.Hi, HiTy, Align.Hi);
    State.recordConverted(Store, ValuePair(Lo, Hi));

  } else if (ICmpInst *Icmp = dyn_cast<ICmpInst>(Inst)) {
    ValuePair Lhs = State.getConverted(Icmp->getOperand(0));
    ValuePair Rhs = State.getConverted(Icmp->getOperand(1));
    switch (Icmp->getPredicate()) {
    case CmpInst::ICMP_EQ:
    case CmpInst::ICMP_NE: {
      Value *Lo = IRB.CreateICmp(Icmp->getUnsignedPredicate(), Lhs.Lo, Rhs.Lo,
                                 Twine(Name, ".lo"));
      Value *Hi = IRB.CreateICmp(Icmp->getUnsignedPredicate(), Lhs.Hi, Rhs.Hi,
                                 Twine(Name, ".hi"));
      Value *Result =
          IRB.CreateBinOp(Instruction::And, Lo, Hi, Twine(Name, ".result"));
      State.recordConverted(Icmp, Result);
      break;
    }

    // TODO(jfb): Implement the following cases.
    case CmpInst::ICMP_UGT:
    case CmpInst::ICMP_UGE:
    case CmpInst::ICMP_ULT:
    case CmpInst::ICMP_ULE:
    case CmpInst::ICMP_SGT:
    case CmpInst::ICMP_SGE:
    case CmpInst::ICMP_SLT:
    case CmpInst::ICMP_SLE:
      errs() << "Comparison: " << *Icmp << "\n";
      report_fatal_error("Comparisons other than equality not supported for"
                         "integer types larger than 64 bit");
    default:
      llvm_unreachable("Invalid integer comparison");
    }

  } else if (SelectInst *Select = dyn_cast<SelectInst>(Inst)) {
    Value *Cond = Select->getCondition();
    ValuePair True = State.getConverted(Select->getTrueValue());
    ValuePair False = State.getConverted(Select->getFalseValue());
    Value *Lo = IRB.CreateSelect(Cond, True.Lo, False.Lo, Twine(Name, ".lo"));
    Value *Hi = IRB.CreateSelect(Cond, True.Hi, False.Hi, Twine(Name, ".hi"));
    State.recordConverted(Select, ValuePair(Lo, Hi));

  } else {
    errs() << "Instruction: " << *Inst << "\n";
    report_fatal_error("Unhandle large integer expansion");
  }
}

bool ExpandLargeIntegers::runOnFunction(Function &F) {
  // Don't support changing the function arguments. Illegal function arguments
  // should not be generated by clang.
#if LLVM_VERSION_MINOR >= 5
  for (const Argument &Arg : F.args())
#else
  for (const Argument &Arg : F.getArgumentList())
#endif
    if (shouldConvert(&Arg))
      report_fatal_error("Function " + F.getName() +
                         " has illegal integer argument");

  // TODO(jfb) This should loop to handle nested forward PHIs.

  ConversionState State;
  DataLayout DL(F.getParent());
  bool Modified = false;
  ReversePostOrderTraversal<Function *> RPOT(&F);
  for (ReversePostOrderTraversal<Function *>::rpo_iterator FI = RPOT.begin(),
                                                           FE = RPOT.end();
       FI != FE; ++FI) {
    BasicBlock *BB = *FI;
    for (Instruction &I : *BB) {
      // Only attempt to convert an instruction if its result or any of its
      // operands are illegal.
      bool ShouldConvert = shouldConvert(&I);
#if LLVM_VERSION_MINOR >= 5
      for (Value *Op : I.operands())
        ShouldConvert |= shouldConvert(Op);
#else
      for (auto it = I.op_begin(); it != I.op_end(); it++)
        ShouldConvert |= shouldConvert(*it);
#endif
      if (ShouldConvert) {
        convertInstruction(&I, State, DL);
        Modified = true;
      }
    }
  }
  State.patchForwardPHIs();
  State.eraseReplacedInstructions();
  return Modified;
}

FunctionPass *llvm::createExpandLargeIntegersPass() {
  return new ExpandLargeIntegers();
}
