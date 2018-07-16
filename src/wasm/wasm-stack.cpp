/*
 * Copyright 2018 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm-stack.h"
#include "ir/iteration.h"

namespace wasm {

void StackIR::optimize(Function* func) {
  class Optimizer {
    StackIR& insts;
    Function* func;

  public:
    Optimizer(StackIR& insts, Function* func) : insts(insts), func(func) {}
    void run() {
      dce();
      local2Stack();
      removeUnneededBlocks();
    }

  private:
    // Passes.

    // Remove unreachable code.
    void dce() {
      bool inUnreachableCode = false;
      for (Index i = 0; i < insts.size(); i++) {
        auto* inst = insts[i];
        if (!inst) continue;
        if (inUnreachableCode) {
          // Does the unreachable code end here?
          if (isControlFlowBarrier(inst)) {
            inUnreachableCode = false;
          } else {
            // We can remove this.
            removeAt(i);
          }
        } else if (inst->type == unreachable) {
          inUnreachableCode = true;
        }
      }
    }

    // If ordered properly, we can avoid a set_local/get_local pair,
    // and use the value directly from the stack, for example
    //    [..produce a value on the stack..]
    //    set_local $x
    //    [..much code..]
    //    get_local $x
    //    call $foo ;; use the value, foo(value)
    // As long as the code in between does not modify $x, and has
    // no control flow branching out, we can remove both the set
    // and the get.
    // We can also leave a value on the stack to flow it out of
    // a block, loop, if, or function body. TODO: verify this
    // happens automatically here
    void local2Stack() {
      // TODO: multipass
      // We maintain a stack of relevant values. This contains:
      //  * a null for each actual value that the value stack would have
      //  * an index of each SetLocal that *could* be on the value
      //    stack at that location.
      const Index null = -1;
      std::vector<Index> values;
      for (Index i = 0; i < insts.size(); i++) {
        auto* inst = insts[i];
        if (!inst) continue;
        // First, consume values from the stack as required.
        auto consumed = getNumConsumedValues(inst);
        while (consumed > 0) {
          assert(values.size() > 0);
          // Whenever we hit a possible stack value, kill it - it would
          // be consumed here, so we can never optimize to it.
          while (values.back() != null) {
            values.pop_back();
            assert(values.size() > 0);
          }
          // Finally, consume the actual value that is consumed here.
          values.pop_back();
          consumed--;
        }
        // TODO: invalidations! Or rather, LocalGraph to match gets and sets.
        // After consuming, see what to do with this. (what about breaks..?)
        if (isConcreteType(inst->type)) {
          if (auto* get = inst->origin->dynCast<GetLocal>()) {
            // This is a potential optimization opportunity! See if we
            // can reach the set.
            if (values.size() > 0) {
              Index j = values.size() - 1;
              while (1) {
                // If there's an actual value in the way, we've failed.
                auto index = values[j];
                if (index == null) break;
                auto* set = insts[index]->origin->cast<SetLocal>();
                if (set->index == get->index) { // FIXME LocalGraph!
                  // Do it! The set and the get can go away, the proper
                  // value is on the stack.
                  insts[index] = nullptr;
                  insts[i] = nullptr;
                  return; // TODO: another pass, or can we continue..?
                }
                // We failed here. Can we look some more?
                if (j == 0) break;
                j--;
              }
            }
          }
          // This is an actual value on the value stack.
          values.push_back(null);
        } else if (inst->origin->is<SetLocal>() && inst->type == none) {
          // This set is potentially optimizable later, add to stack.
          values.push_back(i);
        }
      }
    }

    // There may be unnecessary blocks we can remove: blocks
    // without branches to them are always ok to remove.
    // TODO: a branch to a block in an if body can become
    //       a branch to that if body
    void removeUnneededBlocks() {
    }

    // Utilities.

    // A control flow "barrier" - a point where stack machine
    // unreachability ends.
    bool isControlFlowBarrier(StackInst* inst) {
      switch (inst->op) {
        case StackInst::BlockEnd:
        case StackInst::IfElse:
        case StackInst::IfEnd:
        case StackInst::LoopEnd: {
          return true;
        }
        default: {
          return false;
        }
      }
    }

    // A control flow ending.
    bool isControlFlowEnding(StackInst* inst) {
      switch (inst->op) {
        case StackInst::BlockEnd:
        case StackInst::IfEnd:
        case StackInst::LoopEnd: {
          return true;
        }
        default: {
          return false;
        }
      }
    }

    bool isControlFlow(StackInst* inst) {
      return inst->op != StackInst::Basic;
    }

    // Remove the instruction at index i. If the instruction
    // is control flow, and so has been expanded to multiple
    // instructions, remove them as well.
    void removeAt(Index i) {
      auto* inst = insts[i];
      insts[i] = nullptr;
      if (inst->op == StackInst::Basic) {
        return; // that was it
      }
      auto* origin = inst->origin;
      while (1) {
        i++;
        assert(i < insts.size());
        inst = insts[i];
        insts[i] = nullptr;
        if (inst->origin == origin && isControlFlowEnding(inst)) {
          return; // that's it, we removed it all
        }
      }
    }

    Index getNumConsumedValues(StackInst* inst) {
      if (isControlFlow(inst)) {
        // If consumes 1; that's it.
        if (inst->op == StackInst::IfBegin) {
          return 1;
        }
        return 0;
      }
      // Otherwise, for basic instructions, just count the expression children.
      return ChildIterator(inst->origin).children.size();
    }
  };

  Optimizer(*this, func).run();
}

void StackIR::dump() {
  for (auto* inst : *this) {
    if (!inst) continue;
    std::cout << ' ' << *inst << '\n';
  }
}

} // namespace wasm

