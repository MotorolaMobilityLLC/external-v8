// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_REGISTER_ALLOCATOR_VERIFIER_H_
#define V8_REGISTER_ALLOCATOR_VERIFIER_H_

#include "src/zone-containers.h"

namespace v8 {
namespace internal {
namespace compiler {

class InstructionOperand;
class InstructionSequence;

// The register allocator validator traverses instructions in the instruction
// sequence, and verifies the correctness of machine operand substitutions of
// virtual registers. It collects the virtual register instruction signatures
// before register allocation. Then, after the register allocation pipeline
// completes, it compares the operand substitutions against the pre-allocation
// data.
// At a high level, validation works as follows: we iterate through each block,
// and, in a block, through each instruction; then:
// - when an operand is the output of an instruction, we associate it to the
// virtual register that the instruction sequence declares as its output. We
// use the concept of "FinalAssessment" to model this.
// - when an operand is used in an instruction, we check that the assessment
// matches the expectation of the instruction
// - moves simply copy the assessment over to the new operand
// - blocks with more than one predecessor associate to each operand a "Pending"
// assessment. The pending assessment remembers the operand and block where it
// was created. Then, when the value is used (which may be as a different
// operand, because of moves), we check that the virtual register at the use
// site matches the definition of this pending operand: either the phi inputs
// match, or, if it's not a phi, all the predecessors at the point the pending
// assessment was defined have that operand assigned to the given virtual
// register.
// If a block is a loop header - so one or more of its predecessors are it or
// below - we still treat uses of operands as above, but we record which operand
// assessments haven't been made yet, and what virtual register they must
// correspond to, and verify that when we are done with the respective
// predecessor blocks.
// This way, the algorithm always makes a final decision about the operands
// in an instruction, ensuring convergence.
// Operand assessments are recorded per block, as the result at the exit from
// the block. When moving to a new block, we copy assessments from its single
// predecessor, or, if the block has multiple predecessors, the mechanism was
// described already.

enum AssessmentKind { Final, Pending };

class Assessment : public ZoneObject {
 public:
  AssessmentKind kind() const { return kind_; }

 protected:
  explicit Assessment(AssessmentKind kind) : kind_(kind) {}
  AssessmentKind kind_;

 private:
  DISALLOW_COPY_AND_ASSIGN(Assessment);
};

// PendingAssessments are associated to operands coming from the multiple
// predecessors of a block. We only record the operand and the block, and
// will determine if the way the operand is defined (from the predecessors)
// matches a particular use. This handles scenarios where multiple phis are
// defined with identical operands, and the move optimizer moved down the moves
// separating the 2 phis in the block defining them.
class PendingAssessment final : public Assessment {
 public:
  explicit PendingAssessment(const InstructionBlock* origin,
                             InstructionOperand operand)
      : Assessment(Pending), origin_(origin), operand_(operand) {}

  static const PendingAssessment* cast(const Assessment* assessment) {
    CHECK(assessment->kind() == Pending);
    return static_cast<const PendingAssessment*>(assessment);
  }

  const InstructionBlock* origin() const { return origin_; }
  InstructionOperand operand() const { return operand_; }

 private:
  const InstructionBlock* const origin_;
  InstructionOperand operand_;

  DISALLOW_COPY_AND_ASSIGN(PendingAssessment);
};

// FinalAssessments are associated to operands that we know to be a certain
// virtual register.
class FinalAssessment final : public Assessment {
 public:
  explicit FinalAssessment(int virtual_register,
                           const PendingAssessment* original_pending = nullptr)
      : Assessment(Final),
        virtual_register_(virtual_register),
        original_pending_assessment_(original_pending) {}

  int virtual_register() const { return virtual_register_; }
  static const FinalAssessment* cast(const Assessment* assessment) {
    CHECK(assessment->kind() == Final);
    return static_cast<const FinalAssessment*>(assessment);
  }

  const PendingAssessment* original_pending_assessment() const {
    return original_pending_assessment_;
  }

 private:
  int virtual_register_;
  const PendingAssessment* original_pending_assessment_;

  DISALLOW_COPY_AND_ASSIGN(FinalAssessment);
};

struct OperandAsKeyLess {
  bool operator()(const InstructionOperand& a,
                  const InstructionOperand& b) const {
    return a.CompareCanonicalized(b);
  }
};

// Assessments associated with a basic block.
class BlockAssessments : public ZoneObject {
 public:
  typedef ZoneMap<InstructionOperand, Assessment*, OperandAsKeyLess> OperandMap;
  explicit BlockAssessments(Zone* zone)
      : map_(zone), map_for_moves_(zone), zone_(zone) {}
  void Drop(InstructionOperand operand) { map_.erase(operand); }
  void DropRegisters();
  void AddDefinition(InstructionOperand operand, int virtual_register) {
    auto existent = map_.find(operand);
    if (existent != map_.end()) {
      // Drop the assignment
      map_.erase(existent);
    }
    map_.insert(
        std::make_pair(operand, new (zone_) FinalAssessment(virtual_register)));
  }

  void PerformMoves(const Instruction* instruction);
  void PerformParallelMoves(const ParallelMove* moves);
  void CopyFrom(const BlockAssessments* other) {
    CHECK(map_.empty());
    CHECK_NOT_NULL(other);
    map_.insert(other->map_.begin(), other->map_.end());
  }

  OperandMap& map() { return map_; }
  const OperandMap& map() const { return map_; }
  void Print() const;

 private:
  OperandMap map_;
  OperandMap map_for_moves_;
  Zone* zone_;

  DISALLOW_COPY_AND_ASSIGN(BlockAssessments);
};

class RegisterAllocatorVerifier final : public ZoneObject {
 public:
  RegisterAllocatorVerifier(Zone* zone, const RegisterConfiguration* config,
                            const InstructionSequence* sequence);

  void VerifyAssignment();
  void VerifyGapMoves();

 private:
  enum ConstraintType {
    kConstant,
    kImmediate,
    kRegister,
    kFixedRegister,
    kFPRegister,
    kFixedFPRegister,
    kSlot,
    kFPSlot,
    kFixedSlot,
    kNone,
    kNoneFP,
    kExplicit,
    kSameAsFirst,
    kRegisterAndSlot
  };

  struct OperandConstraint {
    ConstraintType type_;
    int value_;  // subkind index when relevant
    int spilled_slot_;
    int virtual_register_;
  };

  struct InstructionConstraint {
    const Instruction* instruction_;
    size_t operand_constaints_size_;
    OperandConstraint* operand_constraints_;
  };

  typedef ZoneVector<InstructionConstraint> Constraints;

  class DelayedAssessments : public ZoneObject {
   public:
    explicit DelayedAssessments(Zone* zone) : map_(zone) {}

    const ZoneMap<InstructionOperand, int, OperandAsKeyLess>& map() const {
      return map_;
    }

    void AddDelayedAssessment(InstructionOperand op, int vreg) {
      auto it = map_.find(op);
      if (it == map_.end()) {
        map_.insert(std::make_pair(op, vreg));
      } else {
        CHECK_EQ(it->second, vreg);
      }
    }

   private:
    ZoneMap<InstructionOperand, int, OperandAsKeyLess> map_;
  };

  Zone* zone() const { return zone_; }
  const RegisterConfiguration* config() { return config_; }
  const InstructionSequence* sequence() const { return sequence_; }
  Constraints* constraints() { return &constraints_; }

  static void VerifyInput(const OperandConstraint& constraint);
  static void VerifyTemp(const OperandConstraint& constraint);
  static void VerifyOutput(const OperandConstraint& constraint);

  void BuildConstraint(const InstructionOperand* op,
                       OperandConstraint* constraint);
  void CheckConstraint(const InstructionOperand* op,
                       const OperandConstraint* constraint);
  BlockAssessments* CreateForBlock(const InstructionBlock* block);

  void ValidatePendingAssessment(RpoNumber block_id, InstructionOperand op,
                                 BlockAssessments* current_assessments,
                                 const PendingAssessment* assessment,
                                 int virtual_register);
  void ValidateFinalAssessment(RpoNumber block_id, InstructionOperand op,
                               BlockAssessments* current_assessments,
                               const FinalAssessment* assessment,
                               int virtual_register);
  void ValidateUse(RpoNumber block_id, BlockAssessments* current_assessments,
                   InstructionOperand op, int virtual_register);

  Zone* const zone_;
  const RegisterConfiguration* config_;
  const InstructionSequence* const sequence_;
  Constraints constraints_;
  ZoneMap<RpoNumber, BlockAssessments*> assessments_;
  ZoneMap<RpoNumber, DelayedAssessments*> outstanding_assessments_;

  DISALLOW_COPY_AND_ASSIGN(RegisterAllocatorVerifier);
};

}  // namespace compiler
}  // namespace internal
}  // namespace v8

#endif
