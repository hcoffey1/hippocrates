#include <algorithm>
#include <vector>
#include <iomanip>
#include <sstream>
#include <deque>

#include "llvm/IR/CFG.h"

#include "FlowAnalyzer.hpp"

using namespace llvm;
using namespace pmfix;
using namespace std;

#pragma region PmDesc

bool PmDesc::getPointsToSet(const llvm::Value *v,                                  
                            std::unordered_set<const llvm::Value *> &ptsSet) {
    /**                                                                            
     * Using a cache for this dramatically reduces the amount of time spent here,  
     * as the call to "getPointsToSet" has to re-traverse a bunch of internal      
     * data structures to construct the set.                                       
     */                                                                            
    bool ret = true;                                                               
    if (!cache_->count(v)) {                                                
        std::vector<const Value*> rawSet;                                            
        ret = anders_->getResult().getPointsToSet(v, rawSet);                      
        if (ret) { 
            ptsSet.insert(rawSet.begin(), rawSet.end());                                                                         
            (*cache_)[v] = ptsSet;                                              
        }                                                                            
    } else {                                                                       
        ptsSet = (*cache_)[v];                                                
    }                                                                              
                                                                                   
    return ret;
}

PmDesc::PmDesc(Module &m) : anders_(new AndersenAAWrapperPass()),
                            cache_(new AndersenCache()) {
    assert(!anders_->runOnModule(m) && "failed!");
}

void PmDesc::addKnownPmValue(Value *pmv) {
    std::unordered_set<const llvm::Value *> ptsSet;
    assert(getPointsToSet(pmv, ptsSet) && "could not get!");

    if (isa<GlobalValue>(pmv)) pm_globals_.insert(ptsSet.begin(), ptsSet.end());
    else pm_locals_.insert(ptsSet.begin(), ptsSet.end());
}

bool PmDesc::pointsToPm(llvm::Value *pmv) {
    std::unordered_set<const llvm::Value *> ptsSet;
    assert(getPointsToSet(pmv, ptsSet) && "could not get!");

    std::unordered_set<const llvm::Value *> pm_values;
    pm_values.insert(pm_locals_.begin(), pm_locals_.end());
    pm_values.insert(pm_globals_.begin(), pm_globals_.end());

    std::vector<const Value*> inter;

    (void)std::set_intersection(ptsSet.begin(), ptsSet.end(),
                                pm_values.begin(), pm_values.end(), inter.begin());
    return !inter.empty();
}

bool PmDesc::isSubsetOf(const PmDesc &possSuper) {
    // They are subsets if the intersection is equal to the smaller set.
    std::vector<const Value*> gi, li;
    
    (void)std::set_intersection(
        possSuper.pm_globals_.begin(), possSuper.pm_globals_.end(),
        pm_globals_.begin(), pm_globals_.end(), gi.begin());

    (void)std::set_intersection(
        possSuper.pm_locals_.begin(), possSuper.pm_locals_.end(),
        pm_locals_.begin(), pm_locals_.end(), li.begin());

    std::unordered_set<const Value*> gs(gi.begin(), gi.end());
    std::unordered_set<const Value*> ls(li.begin(), li.end());
    return gs == pm_globals_ && ls == pm_locals_;
}

std::string PmDesc::str(int indent) const {
    std::string tmp;
    llvm::raw_string_ostream buffer(tmp);

    std::string istr = "";
    for (int i = 0; i < indent; ++i) istr += "\t";

    buffer << istr << "<PmDesc>\n";
    buffer << istr << "\tNum Locals:  " << pm_locals_.size() << "\n";
    buffer << istr << "\tNum Globals: " << pm_globals_.size() << "\n";
    buffer << istr << "</PmDesc>";

    return buffer.str();
}

#pragma endregion

#pragma region FnContext

FnContext::Shared FnContext::doCall(Function *f, CallBase *cb) {
    if (callBaseCache_->count(cb)) {
        return callBaseCache_->at(cb);
    }

    // Copy, basic setup
    FnContext::Shared nctx = std::make_shared<FnContext>(*this);
    nctx->parent_ = shared_from_this();
    nctx->callStack_.push_back(cb);

    return nctx;
}

FnContext::Shared FnContext::doReturn(ReturnInst *ri) {
    FnContext::Shared p = parent_;
    // Propagate up PM values
    if (Value *v = ri->getReturnValue()) {
        if (pm_.pointsToPm(v)) {
            p->pm_.addKnownPmValue(callStack_.back());
        }
    }
    p->pm_.doReturn(pm_);

    return p;
}

bool FnContext::operator==(const FnContext &f) const {
    // if (first_ != f.first_) return false;
    // if (callStack_.size() != f.callStack_.size()) return false;

    // auto ai = callStack_.begin();
    // auto bi = f.callStack_.begin();
    // for (; ai != callStack_.end() && bi != f.callStack_.end();
    //      ++ai, ++bi) {
    //     if ((*ai)->first_ != (*bi)->first_) return false;
    // }
    
    // return true;
    // return false;
    return callStack_ == f.callStack_;
}

std::string FnContext::str(int indent) const {
    std::string tmp;
    llvm::raw_string_ostream buffer(tmp);

    std::string istr = "";
    for (int i = 0; i < indent; ++i) istr += "\t";

    buffer << istr << "<FnContext>\n";
    buffer << istr << "\tEntries: " << callStack_.size() << "\n";
    buffer << pm_.str(indent + 1) << "\n";
    buffer << istr << "</FnContext>";

    return buffer.str();
}

#pragma endregion

#pragma region ContextNode

ContextBlock::Shared ContextBlock::create(const BugLocationMapper &mapper, 
                                          const TraceEvent &te) {
    // Start from the top down.
    FnContext::Shared parent = FnContext::create(mapper.module());
    // [0] is the current location, which we use to set up the node itself.
    for (int i = te.callstack.size() - 1; i >= 1; --i) {
        const LocationInfo &caller = te.callstack[i];
        const LocationInfo &callee = te.callstack[i-1];

        // errs() << "CALLER: " << caller.str() << "\n";
        // errs() << "CALLEE: " << callee.str() << "\n";

        if (!caller.valid() || !mapper.contains(caller)) {
            continue;
        }

        // The location in the caller calls the function of the callee

        std::list<Instruction*> possibleCallSites;
        for (auto *inst : mapper[caller]) {
            possibleCallSites.push_back(inst);
        }

        assert(possibleCallSites.size() > 0 && "don't know how to handle!");
        assert(possibleCallSites.size() == 1 && "don't know how to handle!");

        Instruction *possible = possibleCallSites.front();
        CallBase *callInst = dyn_cast<CallBase>(possible);
        assert(callInst && "don't know how to handle a non-call!");

        Function *f = callInst->getCalledFunction();
        if (!f) {
            f = mapper.module().getFunction(callee.function);
        }
        assert(f && "don't know what's going on!!");
        
        FnContext::Shared curr = parent->doCall(f, callInst);
        
        parent = curr;
    }

    /**
     * Now, we set up the node!
     */ 
    ContextBlock::Shared node = std::make_shared<ContextBlock>();
    node->ctx = parent;

    const LocationInfo &curr = te.callstack[0];
    // We use this to figure out the first and last instruction in the window.
    std::list<Instruction*> possibleLocs;
    for (auto *inst : mapper[curr]) {
        possibleLocs.push_back(inst);
    }
    assert(possibleLocs.size() > 0 && "don't know how to handle!");
    assert(possibleLocs.size() == 1 && "don't know how to handle!");

    Instruction *possible = possibleLocs.front();
    node->first = possible;
    node->last = possible;
    // -- Scroll forward to find the first instruction.
    while (Instruction *tmp = node->first->getPrevNonDebugInstruction()) {
        if (CallBase *cb = dyn_cast<CallBase>(tmp)) {
            Function *f = cb->getCalledFunction();
            if (f && !f->isDeclaration() && !f->isIntrinsic()) break;
        }
        node->first = tmp;
    }
    // -- Scroll down to find the last instruction.
    while (Instruction *tmp = node->last->getNextNonDebugInstruction()) {
        if (CallBase *cb = dyn_cast<CallBase>(tmp)) {
            Function *f = cb->getCalledFunction();
            if (f && !f->isDeclaration() && !f->isIntrinsic()) break;
        }
        node->last = tmp;
    }

    // We also use the trace event itself to initialize some PM values.
    errs() << "\t\tTODO DO ME\n";

    return node;
}

std::string ContextBlock::str(int indent) const {
    std::string tmp;
    llvm::raw_string_ostream buffer(tmp);

    std::string istr = "";
    for (int i = 0; i < indent; ++i) istr += "\t";

    buffer << istr << "<ContextBlock>\n";
    buffer << istr << "\tFirst: " << *first << "\n";
    buffer << istr << "\tLast:  " << *last << "\n";
    buffer << ctx->str(indent + 1) << "\n";
    buffer << istr << "</ContextBlock>";

    return buffer.str();
}

#pragma endregion

#pragma region ContextGraph

// template <typename T>
// std::list<
//     std::shared_ptr<ContextGraph<T>::GraphNode>
// > 
// ContextGraph<T>::constructSuccessors(ContextGraph<T>::GraphNodePtr node) {
//     std::list<ContextGraph::GraphNodePtr> successors;
//     return successors;
// }

template <typename T>
std::list<typename ContextGraph<T>::GraphNodePtr> 
ContextGraph<T>::constructSuccessors(ContextGraph<T>::GraphNodePtr node) {
    std::list<ContextGraph::GraphNodePtr> successors;

    /**
     * Normally, a basic block has it's own branching successors. However,
     * we also want to capture function calls. So, we will split basic blocks
     * at calls.
     * 
     * We also want to capture return instructions.
     */
    Instruction *i = node->block->first;
    while ((i = i->getNextNonDebugInstruction())) {
        if (CallBase *cb = dyn_cast<CallBase>(i)) {
            Function *fn = cb->getCalledFunction();

            if (fn && fn->isIntrinsic()) continue;
            else if (fn && fn->isDeclaration()) continue;
            else if (fn == first_->getFunction()) {
                // TODO: recursion protection;
                assert(false && "TODO RECURSE");
            }

            // this sets the last, and the successor is the call.
            last_ = cb;
            Instruction *fnFirst = &fn->getEntryBlock().front();
            FnContext *called = new FnContext(this, fnFirst);
            successors_.push_back(called);
            return;
        }

        if (ReturnInst *ri = dyn_cast<ReturnInst>(i)) {
            last_ = ri;
            // This would mean that we're returning past the end of our scope.
            // Remember, it's a limited graph.
            FnContext *parent = callStack_.back();
            if (!parent) {
                return;
            }

            assert(parent->last_ && "last not set!!!");

            /**
             * We want essentially set the parent as the successor, but
             * make sure to move the "first" instruction cursor so that
             * we don't loop over this too much.
             */
            if (callStack_.back()) {
                
                Instruction *ni = parent->last_->getNextNonDebugInstruction();
                if (!ni) {
                    assert(false && "TODO! Likely need to do something here.");
                }
                auto *rchild = new FnContext(parent->callStack_, ni);
                successors_.push_back(rchild);
            }

            // No successors.
            return;
        }
    }
    
    /**
     * Now, we get the successor basic block.
     */
    last_ = first_->getParent()->getTerminator();
    for (BasicBlock *succ : llvm::successors(first_->getParent())) {
        FnContext *sctx = new FnContext(callStack_, succ->getFirstNonPHI());
        successors_.push_back(sctx);
    }
    
    return successors;
}

template <typename T>
void ContextGraph<T>::construct(ContextBlock::Shared end) {
    std::deque<std::shared_ptr<GraphNode>> frontier(roots.begin(), roots.end());

    size_t nnodes = roots.size();
    /**
     * For each node:
     * 1. Get the successing function contexts
     * 2. Construct nodes for each child context
     * 3. Add as children if conditions work.
     */
    while (frontier.size()) {
        ContextGraph::GraphNodePtr n = frontier.front();
        frontier.pop_front();

        // Pre-check
        errs() << "------\n";
        if (*n->block == *end) {
            errs() << "End traversal\n";
            leaves.push_back(n);
            errs() << "------\n";
            continue;
        } else {
            errs() << "NE:\n" << end->str() << "\nEND NE\n";
        }

        // Construct successors.
        // n->ctx->constructSuccessors();

        // errs() << "Traverse " << n->ctx->str() << "\n";

        // for (FnContext *child_ctx : n->ctx->successors()) {
        //     errs() << "\t child: \n" << child_ctx->str() << "\n";
        //     nnodes++;
        //     Node *child = new Node(child_ctx);
        //     n->addChild(child);

        //     frontier.push_back(child);
        // }

        // if (n->children.empty()) {
        //     errs() << "no kids!\n";
        //     leaves.push_back(n);
        // }

        // errs() << "------\n";
    }

    errs() << "<<< Created " << nnodes << " nodes! >>>\n";
    errs() << "<<< Have " << leaves.size() << " leaves! >>>\n";
}

template <typename T>
ContextGraph<T>::ContextGraph(const BugLocationMapper &mapper, 
                              const TraceEvent &start, 
                              const TraceEvent &end) {
    errs() << "CONSTRUCT ME\n\n";

    ContextBlock::Shared sblk = ContextBlock::create(mapper, start);
    ContextBlock::Shared eblk = ContextBlock::create(mapper, end);
    errs() << sblk->str() << "\n";
    errs() << eblk->str() << "\n";

    assert(*sblk == *eblk);

    errs() << "\nEND CONSTRUCT\n";
    // root = new Node(start);

    auto root = std::make_shared<ContextGraph::GraphNode>(sblk);
    roots.push_back(root);

    construct(eblk);

    // Validate that the leaf nodes are all what we expect them to be.
    assert(leaves.size() >= 1 && "Did not construct leaves!");
    for (ContextGraph::GraphNodePtr n : leaves) {
        if (*n->block != *eblk && !n->isTerminator()) {
            errs() << (*n->block != *eblk) << " && " << 
                (!n->isTerminator()) << "\n";
            assert(false && "wat");
        }
    }
}

template struct pmfix::ContextGraph<bool>;

#pragma endregion

#pragma region FlowAnalyzer


#pragma endregion