#include "BugFixer.hpp"
#include "FlowAnalyzer.hpp"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IRBuilder.h"

using namespace pmfix;
using namespace llvm;

#pragma region BugFixer

bool BugFixer::addFixToMapping(Instruction *i, FixDesc desc) {
    if (!fixMap_.count(i)) {
        errs() << "DING\n";
        fixMap_[i] = desc;
        return true;
    } else if (fixMap_[i] == desc) {
        errs() << "DONG\n";
        return false;
    }
    errs() << "NOPE\n";

    if (fixMap_[i].type == ADD_FLUSH_ONLY && desc.type == ADD_FENCE_ONLY) {
        fixMap_[i].type = ADD_FLUSH_AND_FENCE;
        return true;
    } else if (fixMap_[i].type == ADD_FENCE_ONLY && desc.type == ADD_FLUSH_ONLY) {
        fixMap_[i].type = ADD_FLUSH_AND_FENCE;
        return true;
    }

    if ((fixMap_[i].type == ADD_FLUSH_ONLY || fixMap_[i].type == ADD_FLUSH_AND_FENCE) &&
        desc.type == REMOVE_FLUSH_ONLY) {
        assert(false && "conflicting solutions!");
    }
    // (iangneal): future work
    // if (fixMap_[i] == REMOVE_FLUSH_ONLY && t == REMOVE_FENCE_ONLY) {
    //     fixMap_[i] = REMOVE_FLUSH_AND_FENCE;
    //     return true;
    // } else if (fixMap_[i] == REMOVE_FENCE_ONLY && t == REMOVE_FLUSH_ONLY) {
    //     fixMap_[i] = REMOVE_FLUSH_AND_FENCE;
    //     return true;
    // }

    assert(false && "what");
    return false;
}

/**
 * If something is not persisted, that means one of three things:
 * 1. It is missing a flush.
 * - In this case, we need to insert a flush between the ASSIGN and 
 * it's nearest FENCE.
 * - Needs to be some check-up operation.
 * 
 * 2. It is missing a fence.
 * - In this case, we need to insert a fence after the ASSIGN and it's 
 * FLUSH.
 * 
 * 3. It is missing a flush AND a fence.
 */
bool BugFixer::handleAssertPersisted(const TraceEvent &te, int bug_index) {
    bool missingFlush = false;
    bool missingFence = true;
    // Need this so we know where the eventual fix will go.
    int lastOpIndex = -1;

    // First, determine which case we are in by going backwards.
    for (int i = bug_index - 1; i >= 0; i--) {
        const TraceEvent &event = trace_[i];
        if (!event.isOperation()) continue;

        assert(event.addresses.size() <= 1 && 
                "Don't know how to handle more addresses!");
        if (event.addresses.size()) {
            errs() << "Address: " << event.addresses.front().address << "\n";
            errs() << "Length:  " << event.addresses.front().length << "\n";
            assert(event.addresses.front().isSingleCacheLine() && 
                    "Don't know how to handle multi-cache line operations!");

            if (event.type == TraceEvent::STORE &&
                event.addresses.front() == te.addresses.front()) {
                missingFlush = true;
                lastOpIndex = i;
                break;
            } else if (event.type == TraceEvent::FLUSH &&
                        event.addresses.front().overlaps(te.addresses.front())) {
                assert(missingFence == true &&
                        "Shouldn't be a bug in this case, has flush and fence");
                lastOpIndex = i;
                break;
            }
        } else if (event.type == TraceEvent::FENCE) {
            missingFence = false;
            missingFlush = true;
        }
    }

    errs() << "\t\tMissing Flush? : " << missingFlush << "\n";
    errs() << "\t\tMissing Fence? : " << missingFence << "\n";
    errs() << "\t\tLast Operation : " << lastOpIndex << "\n";

    assert(lastOpIndex >= 0 && "Has to had been assigned at least!");

    // Find where the last operation was.
    const TraceEvent &last = trace_[lastOpIndex];
    errs() << "\t\tLocation : " << last.location.str() << "\n";
    assert(mapper_[last.location].size() && "can't have no instructions!");
    bool added = false;
    for (Instruction *i : mapper_[last.location]) {
        assert(i && "can't be null!");
        errs() << "\t\tInstruction : " << *i << "\n";
        
        bool res = false;
        FixDesc desc;
        if (missingFlush && missingFence) {
            desc.type = ADD_FLUSH_AND_FENCE;
            res = addFixToMapping(i, desc);
        } else if (missingFlush) {
            desc.type = ADD_FLUSH_ONLY;
            res = addFixToMapping(i, desc);
        } else if (missingFence) {
            desc.type = ADD_FENCE_ONLY;
            res = addFixToMapping(i, desc);
        }

        // Have to do it this way, otherwise it short-circuits.
        added = res || added;
    }
    
    return added;      
}

bool BugFixer::handleAssertOrdered(const TraceEvent &te, int bug_index) {
    errs() << "\tTODO: implement " << __FUNCTION__ << "!\n";
    return false;
}

bool BugFixer::handleRequiredFlush(const TraceEvent &te, int bug_index) {
    /**
     * Step 1: find the redundant flush and the original flush.
     */

    int redundantIdx = -1;
    int originalIdx = -1;

    for (int i = bug_index - 1; i >= 0; i--) {
        if (redundantIdx != -1 && originalIdx != -1) break;
        const TraceEvent &event = trace_[i];
        if (!event.isOperation()) continue;

        assert(event.addresses.size() <= 1 && 
                "Don't know how to handle more addresses!");
        if (event.addresses.size()) {
            errs() << "Address: " << event.addresses.front().address << "\n";
            errs() << "Length:  " << event.addresses.front().length << "\n";
            assert(event.addresses.front().isSingleCacheLine() && 
                    "Don't know how to handle multi-cache line operations!");

            if (event.type == TraceEvent::FLUSH &&
                event.addresses.front() == te.addresses.front()) {
                if (redundantIdx == -1) redundantIdx = i;
                else {
                    originalIdx = i;
                    break;
                }
            } else if (event.type == TraceEvent::FLUSH &&
                        event.addresses.front().overlaps(te.addresses.front())) {
                assert(false && "wat");
                break;
            }
        } 
    }

    errs() << "\tRedundant Index : " << redundantIdx << "\n";
    errs() << "\tOriginal Index : " << originalIdx << "\n";

    assert(redundantIdx >= 0 && "Has to have a redundant index!");
    assert(originalIdx >= 0 && "Has to have a redundant index!");

    /**
     * Step 2: Figure out how we can fix this bug.
     * 
     * Options:
     *  - If the two flushes are in the same function context...
     *      - If the redundant is dominated by the original, delete the redundant.
     *      - If the redundant post-dominates the original, delete the original.
     *      - If there is a common post-dominator, delete both and insert in the
     *        common post-dominator.
     * 
     *  - If the two flushes are NOT in the same function context, then we have
     *  to do some complicated crap.
     *      - TODO
     * 
     * Otherwise, abort.
     */

    const TraceEvent &orig = trace_[originalIdx];
    const TraceEvent &redt = trace_[redundantIdx];

    errs() << "Original: " << orig.str() << "\n";
    errs() << "Redundant: " << redt.str() << "\n";

    // Are they in the same context?
    if (TraceEvent::callStacksEqual(orig, redt)) {
        errs() << "\teq stack!\n";
    } else {
        errs() << "\tneq!!\n";
    }

    // ContextGraph<bool> graph(mapper_, orig, redt);
    FlowAnalyzer f(module_, mapper_, orig, redt);
    errs() << "Always redundant? " << f.alwaysRedundant() << "\n";

    // Then we can just remove the redundant flush.
    bool res = false;
    for (Instruction *i : mapper_[redt.location]) {
        if (f.alwaysRedundant()) {
            FixDesc desc;
            desc.type = REMOVE_FLUSH_ONLY;
            res = addFixToMapping(i, desc);
        } else {
            std::list<Instruction*> redundantPaths = f.redundantPaths();
            if (redundantPaths.size()) {
                // Set dependent of the real fix
                Instruction *prev = i;
                for (Instruction *a : redundantPaths) {
                    FixDesc d(ADD_FLUSH_CONDITION, prev);
                    bool ret = addFixToMapping(a, d);
                    assert(ret && "wat");
                    prev = a; 
                }

                FixDesc remove(REMOVE_FLUSH_CONDITIONAL, prev);
                bool ret = addFixToMapping(i, remove);
                res = res || ret;
            }
        }
    }

    return res;
}

bool BugFixer::computeAndAddFix(const TraceEvent &te, int bug_index) {
    assert(te.isBug && "Can't fix a not-a-bug!");

    switch(te.type) {
        case TraceEvent::ASSERT_PERSISTED: {
            errs() << "\tPersistence Bug (Universal Correctness)!\n";
            assert(te.addresses.size() == 1 &&
                "A persist assertion should only have 1 address!");
            assert(te.addresses.front().isSingleCacheLine() &&
                "Don't know how to handle non-standard ranges which cross lines!");
            return handleAssertPersisted(te, bug_index);
        }
        case TraceEvent::REQUIRED_FLUSH: {
            errs() << "\tPersistence Bug (Universal Performance)!\n";
            assert(te.addresses.size() > 0 &&
                "A redundant flush assertion needs an address!");
            assert(te.addresses.size() == 1 &&
                "A persist assertion should only have 1 address!");
            assert(te.addresses.front().isSingleCacheLine() &&
                "Don't know how to handle non-standard ranges which cross lines!");
            return handleRequiredFlush(te, bug_index);
        }
        default: {
            errs() << "Not yet supported: " << te.typeString << "\n";
            return false;
        }
    }
}

bool BugFixer::fixBug(FixGenerator *fixer, Instruction *i, FixDesc desc) {
    switch (desc.type) {
        case ADD_FLUSH_ONLY: {
            Instruction *n = fixer->insertFlush(i);
            assert(n && "could not ADD_FLUSH_ONLY");
            break;
        }
        case ADD_FENCE_ONLY: {
            Instruction *n = fixer->insertFence(i);
            assert(n && "could not ADD_FENCE_ONLY");
            break;
        }
        case ADD_FLUSH_AND_FENCE: {
            Instruction *n = fixer->insertFlush(i);
            assert(n && "could not add flush of FLUSH_AND_FENCE");
            n = fixer->insertFence(n);
            assert(n && "could not add fence of FLUSH_AND_FENCE");
            break;
        }
        case REMOVE_FLUSH_ONLY: {
            bool success = fixer->removeFlush(i);
            assert(success && "could not remove flush of REMOVE_FLUSH_ONLY");
            break;
        }
        default: {
            errs() << "UNSUPPORTED: " << desc.type << "\n";
            assert(false && "not handled!");
        }
    }

    return true;
}

bool BugFixer::runFixMapOptimization(void) {
    errs() << "\tTODO: implement " << __FUNCTION__ << "!\n";
    return false;
}

bool BugFixer::doRepair(void) {
    bool modified = false;

    /**
     * Select the bug fixer based on the source of the bug report. Mostly 
     * differentiates between tools which require assertions (PMTEST) and 
     * everything else.
     */
    FixGenerator *fixer = nullptr;
    switch (trace_.getSource()) {
        case TraceEvent::PMTEST: {
            PMTestFixGenerator pmtestFixer(module_);
            fixer = &pmtestFixer;
            break;
        }
        case TraceEvent::GENERIC: {
            GenericFixGenerator genericFixer(module_);
            fixer = &genericFixer;
            break;
        }
        default: {
            errs() << "unsupported!\n";
            exit(-1);
        }
    }

    /**
     * Step 1.
     * 
     * Now, we find all the fixes.
     */
    for (int bug_index : trace_.bugs()) {
        errs() << "Bug Index: " << bug_index << "\n";
        bool addedFix = computeAndAddFix(trace_[bug_index], bug_index);
        if (addedFix) {
            errs() << "\tAdded a fix!\n";
        } else {
            errs() << "\tDid not add a fix!\n";
        }
    }

    bool couldOpt = runFixMapOptimization();
    if (couldOpt) {
        errs() << "Was able to optimize!\n";
    } else {
        errs() << "Was not able to perform fix map optimizations!\n";
    }

    /**
     * Step 3.
     * 
     * The final step. Now, we actually do the fixing.
     */
    for (auto &p : fixMap_) {
        bool res = fixBug(fixer, p.first, p.second);
        modified = modified || res;
    }

    return modified;
}

#pragma endregion
