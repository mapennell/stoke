// Copyright 2013-2016 Stanford University
//
// Licensed under the Apache License, Version 2.0 (the License);
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an AS IS BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef STOKE_SRC_VALIDATOR_OBLIGATION_CHECKER_H
#define STOKE_SRC_VALIDATOR_OBLIGATION_CHECKER_H

#include <functional>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>

#include "gtest/gtest_prod.h"

#include "src/cfg/cfg.h"
#include "src/cfg/paths.h"
#include "src/ext/x64asm/include/x64asm.h"
#include "src/solver/smtsolver.h"
#include "src/symstate/dereference_info.h"
#include "src/symstate/memory/cell.h"
#include "src/symstate/memory/flat.h"
#include "src/symstate/memory/arm.h"
#include "src/validator/data_collector.h"
#include "src/validator/invariant.h"
#include "src/validator/filters/default.h"
#include "src/validator/filters/bound_away.h"

//#define DEBUG_CHECKER_PERFORMANCE

#ifdef DEBUG_CHECKER_PERFORMANCE
#include "src/solver/z3solver.h"
#endif

namespace stoke {

class ObligationChecker {
  friend class ObligationCheckerBaseTest;
  FRIEND_TEST(ObligationCheckerBaseTest, WcpcpyA);
  FRIEND_TEST(ObligationCheckerBaseTest, ProveMemoryObligation);
  FRIEND_TEST(ObligationCheckerBaseTest, ProveMemoryObligationFail);
  FRIEND_TEST(ObligationCheckerBaseTest, AssumeMemoryNull);
  FRIEND_TEST(ObligationCheckerBaseTest, AssumeMemoryNullFail);
  FRIEND_TEST(ObligationCheckerBaseTest, AssumeAndProve);
  FRIEND_TEST(ObligationCheckerBaseTest, AssumeAndProveFail);
  FRIEND_TEST(ObligationCheckerBaseTest, NeedMemoryInToProveMemoryOut);
  FRIEND_TEST(ObligationCheckerBaseTest, NeedMemoryInToProveMemoryOut2);
  FRIEND_TEST(ObligationCheckerBaseTest, NeedMemoryInToProveEquality);

public:

  enum AliasStrategy {
    BASIC,             // enumerate all cases, attempt to bound it (SOUND)
    FLAT,              // model memory as an array in the SMT solver (SOUND)
    ARM,               // improved implementation of "STRING" (SOUND)
    ARMS_RACE          // run ARM and FLAT in parallel (SOUND)
  };

  struct Result {
    bool verified;
    bool has_ceg;

    bool has_error;
    std::string error_message;

    CpuState target_ceg;
    CpuState rewrite_ceg;
    CpuState target_final_ceg;
    CpuState rewrite_final_ceg;

    Result() { }
  };

  typedef std::function<void (Result&, void*)> Callback;

  ObligationChecker() 
  {
    set_alias_strategy(AliasStrategy::FLAT);
    set_nacl(false);
    set_basic_block_ghosts(true);
    set_fixpoint_up(false);
  }

  ObligationChecker(const ObligationChecker& oc) 
  {
    basic_block_ghosts_ = oc.basic_block_ghosts_;
    nacl_ = oc.nacl_;
    fixpoint_up_ = oc.fixpoint_up_;
    alias_strategy_ = oc.alias_strategy_;
  }

  virtual ~ObligationChecker() {
  }

  /** Set strategy for aliasing */
  ObligationChecker& set_alias_strategy(AliasStrategy as) {
    alias_strategy_ = as;
    return *this;
  }

  AliasStrategy get_alias_strategy() {
    return alias_strategy_;
  }

  ObligationChecker& set_fixpoint_up(bool b) {
    fixpoint_up_ = b;
    return *this;
  }

  /** If every memory reference in your code is of the form (r15,r*x,1), then
    setting this option to 'true' is logically equivalent to adding constraints
    that bound the index register away from the top/bottom of the 32-bit
    address space.  It is unsound for NaCl code only if you have a memory
    dereference of (r15,r*x,k) where k = 2, 4 or 8.  This does not come up in
    any of our NaCl examples, and sould be rare to find since no compilers
    generate code that use an index besides 1 for NaCl; and STOKE won't do this
    transformation. */
  ObligationChecker& set_nacl(bool b) {
    nacl_ = b;
    return *this;
  }

  /** Turn on per-basic block ghost variables.  This will track a ghost variable
    for each basic block that gets incremented by one on each execution. */
  ObligationChecker& set_basic_block_ghosts(bool b) {
    basic_block_ghosts_ = b;
    return *this;
  }

  /** Turn checking into a synchronous operation. */
  Result check_wait(const Cfg& target, const Cfg& rewrite,
                    Cfg::id_type target_block, Cfg::id_type rewrite_block,
                    const CfgPath& p, const CfgPath& q,
                    Invariant& assume, Invariant& prove,
                    const std::vector<std::pair<CpuState, CpuState>>& testcases) {

    std::atomic<bool> await_complete(false);
    Result await_result;
    
    Callback callback = [&] (Result& result, void*) {
      await_result = result;
      await_complete.store(true, std::memory_order_release);
    };

    check(target, rewrite, target_block, rewrite_block, p, q, assume, prove, testcases,
          callback, NULL);

    while(!await_complete.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    
    return await_result;
  }

  /** Check.  This performs the requested obligation check, and depending on the implementation may
    choose to either:
      (1) block, call the callback (in the same thread/process), and then return; or
      (2) start an asynchronous job (which will later invoke the callback) and return; or
      (3) block, then start an asyncrhonous job (which will call the callback) and return. */
  virtual void check(const Cfg& target, const Cfg& rewrite,
                     Cfg::id_type target_block, Cfg::id_type rewrite_block,
                     const CfgPath& p, const CfgPath& q,
                     Invariant& assume, Invariant& prove,
                     const std::vector<std::pair<CpuState, CpuState>>& testcases,
                     Callback& callback,
                     void* optional = NULL) = 0;

  /** Below are hacks due to legacy non-existence of a type hierarchy for obligation checkers. */
  enum JumpType {
    NONE, // jump target is the fallthrough
    FALL_THROUGH,
    JUMP
  };

  /** Get the filter */
  virtual Filter& get_filter() = 0;

  /** Is there a jump in the path following this basic block? */
  static JumpType is_jump(const Cfg&, const Cfg::id_type start, const CfgPath& P, size_t i);


protected:

  AliasStrategy alias_strategy_;
  bool basic_block_ghosts_;
  bool nacl_;
  bool fixpoint_up_;

};



} // namespace stoke

#endif
