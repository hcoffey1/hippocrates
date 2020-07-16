#include <algorithm>
#include <vector>
#include <iomanip>
#include <sstream>
#include <deque>
#include <utility>

#include "llvm/IR/CFG.h"

#include "FlowAnalyzer.hpp"
#include "PassUtils.hpp"

using namespace llvm;
using namespace pmfix;
using namespace std;

#pragma region PmDesc

SharedAndersen PmDesc::anders_(nullptr);
SharedAndersenCache PmDesc::cache_(nullptr);

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

PmDesc::PmDesc(Module &m) {
    if (!anders_) {
        anders_ = std::make_shared<AndersenAAWrapperPass>();
        assert(!anders_->runOnModule(m) && "failed!");
    }
    if (!cache_) {
        cache_ = std::make_shared<AndersenCache>();
    }
}

void PmDesc::addKnownPmValue(Value *pmv) {
    std::unordered_set<const llvm::Value *> ptsSet;
    assert(getPointsToSet(pmv, ptsSet) && "could not get!");
    assert(ptsSet.size() && "no points to!");

    if (isa<GlobalValue>(pmv)) pm_globals_.insert(ptsSet.begin(), ptsSet.end());
    else pm_locals_.insert(ptsSet.begin(), ptsSet.end());
}

bool PmDesc::pointsToPm(llvm::Value *pmv) {
    std::unordered_set<const llvm::Value *> ptsSet;
    assert(getPointsToSet(pmv, ptsSet) && "could not get!");

    std::unordered_set<const llvm::Value *> pm_values;
    pm_values.insert(pm_locals_.begin(), pm_locals_.end());
    pm_values.insert(pm_globals_.begin(), pm_globals_.end());

    // errs() << "ptsSet size: " << ptsSet.size() << "\n";
    // errs() << "pm_values size: " << pm_values.size() << "\n";

    std::unordered_set<const llvm::Value*> both, intersect;
    both.insert(pm_values.begin(), pm_values.end());
    both.insert(ptsSet.begin(), ptsSet.end());

    // a form of set intersection
    // (void)std::set_intersection(ptsSet.begin(), ptsSet.end(),
    //                             pm_values.begin(), pm_values.end(), inter.begin());

    for (const Value *v : both) {
        if (pm_values.count(v) && ptsSet.count(v)) {
            intersect.insert(v);
        }
    }

    return !intersect.empty();
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

ContextBlock::Shared ContextBlock::create(FnContext::Shared ctx, 
                                          llvm::Instruction *first,
                                          llvm::Instruction *trace) {
    /**
     * Now, we set up the node!
     */ 
    ContextBlock::Shared node = std::make_shared<ContextBlock>();
    node->ctx = ctx;
    node->first = first;
    node->last = first;
    node->traceInst = trace;

    // -- Scroll down to find the last instruction.
    while (Instruction *tmp = node->last->getNextNonDebugInstruction()) {
        if (CallBase *cb = dyn_cast<CallBase>(tmp)) {
            Function *f = cb->getCalledFunction();
            if (f && !f->isDeclaration() && !f->isIntrinsic()) break;
        }
        node->last = tmp;
    }

    errs() << "------\n";
    errs() << node->str() << "\n";
    errs() << "------\n";

    return node;
}

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
     * Now, we set up arguments so we can call the other create() function.
     */ 

    const LocationInfo &curr = te.callstack[0];
    if (!mapper.contains(curr)) {
        errs() << "stack[0] " << curr.str() << "\n";
        errs() << "location " << te.location.str() << "\n";
        assert(false && "does not contain!!!");
    }
    // We use this to figure out the first and last instruction in the window.
    std::list<Instruction*> possibleLocs;
    for (auto *inst : mapper[curr]) {
        possibleLocs.push_back(inst);
    }
    assert(possibleLocs.size() > 0 && "don't know how to handle!");
    assert(possibleLocs.size() == 1 && "don't know how to handle!");

    Instruction *nodeFirst = possibleLocs.front();
    Instruction *traceInst = nodeFirst;

    /**
     * Set up the PmDesc in the FnContext
     */
    auto pmVals = te.pmValues(mapper);
    assert(pmVals.size() > 0 && "wat");
    for (Value *pmVal : pmVals) {
        errs() << "Add:" << *pmVal << "\n";
        parent->pm().addKnownPmValue(pmVal);
    }

    // -- Scroll back to find the first instruction.
    while (Instruction *tmp = nodeFirst->getPrevNonDebugInstruction()) {
        if (CallBase *cb = dyn_cast<CallBase>(tmp)) {
            Function *f = cb->getCalledFunction();
            if (f && !f->isDeclaration() && !f->isIntrinsic()) break;
        }
        nodeFirst = tmp;
    }

    return create(parent, nodeFirst, traceInst);
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

template <typename T>
std::list<typename ContextGraph<T>::GraphNodePtr> 
ContextGraph<T>::constructSuccessors(ContextGraph<T>::GraphNodePtr node) {
    /**
     * TODO: We can use the caching mechanism as a way of doing loop detection.
     */
    std::list<ContextGraph::GraphNodePtr> finalSuccessors;
    node->constructed = true;

    /**
     * What we want to do here is collect FnContext, Instruction tuples.
     * If they're in the cache, then we're all good, otherwise we should 
     * create a new graph node.
     */
    typedef std::pair<FnContext::Shared, Instruction*> SuccType;
    std::list<SuccType> successors;

    Instruction *last = node->block->last;

    /**
     * If the last instruction is a return instruction, then the only successor
     * is the instruction after the call base.
     */

    if (ReturnInst *ri = dyn_cast<ReturnInst>(last)) {
        auto newCtx = node->block->ctx->doReturn(ri);
        // The next instruction isn't too complicated
        CallBase *cb = node->block->ctx->caller();
        Instruction *next = cb->getNextNonDebugInstruction();
        assert(next && "bad assumptions!");
        successors.emplace_back(newCtx, next);
    }

    /**
     * If this is a call instruction, we need to do the call.
     */
    else if (CallBase *cb = dyn_cast<CallBase>(last)) {
        // We just need the function.
        Function *f = cb->getCalledFunction();
        assert(f && "don't know how to handle this yet!");
        auto newCtx = node->block->ctx->doCall(f, cb);
        Instruction *next = &f->getEntryBlock().front();
        successors.emplace_back(newCtx, next);
    }

    /**
     * If this is the last instruction in the basic block, then we get the 
     * successors.
     */
    else if (last->isTerminator()) {
        errs() << "LAST TERM " << *last << "\n";
        for (BasicBlock *succ : llvm::successors(last->getParent())) {
            successors.emplace_back(node->block->ctx, 
                                    succ->getFirstNonPHIOrDbgOrLifetime());
            errs() << "HEY HEY HEY " << *succ->getFirstNonPHIOrDbgOrLifetime() << "\n";
        }
    }

    /**
     * This case doesn't make any sense, so we will fail hard.
     */
    else {
        assert(false && "wat");
    }

    for (SuccType &st : successors) {
        if (nullptr != nodeCache_[st.first][st.second]) {
            errs() << "CACHE HIT BRONT " << *last << "\n";
            finalSuccessors.push_back(nodeCache_[st.first][st.second]);
        } else {
            // Need a new context block
            auto newCtx = ContextBlock::create(st.first, st.second, st.second);
            auto newNode = std::make_shared<ContextGraph::GraphNode>(newCtx);
            finalSuccessors.push_back(newNode);
            nodeCache_[st.first][st.second] = newNode;
        }
    }

    return finalSuccessors;
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
        errs() << "------B\n";
        errs() << "Traverse " << n->block->str() << "\n";
        if (*n->block == *end) {
            errs() << "equals end!!! End traversal\n";
            // This counts as "construction"
            if (n->constructed) {
                errs() << "Already constructed! DO NOTHING\n";
                nnodes--;
            } else {
                n->constructed = true;
                // Update the trace instruction too
                n->block->traceInst = end->traceInst;
                leaves.push_back(n);
            }
            
            errs() << "------E\n";
            continue;
        } else {
            errs() << "NE:\n" << end->str() << "\nEND NE\n";
        }

        // Construct successors.
        auto successors = constructSuccessors(n);

        for (auto childNode : successors) {
            // Set parent-child relations
            n->children.insert(childNode);
            childNode->parents.insert(n);

            /**
             * If a child has already been constructed, than means we have a 
             * loop! So, we don't add it back to the frontier.
             */
            if (!childNode->constructed) {
                nnodes++;
                frontier.push_back(childNode);
            }
        }

        if (n->isTerminator()) {
            errs() << "no kids!\n";
            leaves.push_back(n);
        }
        errs() << "------E\n";
    }

    errs() << "<<< Created " << nnodes << " nodes! >>>\n";
    errs() << "<<< Have " << roots.size() << " roots! >>>\n";
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

    errs() << "\nEND CONSTRUCT\n";

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
template struct pmfix::ContextGraph<pmfix::FlowAnalyzer::Info>;

#pragma endregion

#pragma region FlowAnalyzer

bool FlowAnalyzer::interpret(ContextGraph<Info>::GraphNodePtr node,
                             Instruction *start, Instruction *end) {
    Info &info = node->metadata;
    PmDesc &pm = node->block->ctx->pm();
    assert(start->getParent() == end->getParent() && "not in same BB!");

    if (info.updated) return !info.isNotRedundant;

    // errs() << "Interpret start: " << *start << "\n";
    // errs() << "Interpret end:   " << *end << "\n";

    Instruction *i = start;
    bool isStillRedt = true;
    // We do != end + 1 since end is inclusive
    while (i != end->getNextNonDebugInstruction()) {
        if (auto *si = dyn_cast<StoreInst>(i)) {
            Value *v = si->getPointerOperand();
            if (pm.pointsToPm(v)) {
                /**
                 * TODO: if there's a flush which flushes this exactly, we're 
                 * okay, otherwise there's no hope.
                 */
                isStillRedt = false;
                // errs() << "spoiler:" << *v << "\n";
            }
        } else if (auto *cb = dyn_cast<CallBase>(i)) {
            Function *flushFn = utils::getFlush(cb);
            if (flushFn && !isStillRedt) {
                errs() << *cb << "\n";
                assert(false && "TODO");
            }
            
        }

        i = i->getNextNonDebugInstruction();
    }

    info.isNotRedundant = !isStillRedt;
    info.updated = true;

    return isStillRedt;
}

bool FlowAnalyzer::alwaysRedundant() {
    bool redundant = true;
    for (auto nptr : graph_.roots) {
        
        // For each child, we need to traverse, interpret, and so on.

        // -- Special case. For one node, it's always redundant.
        if (nptr->children.empty()) {
            continue;
        }

        bool r = interpret(nptr, nptr->block->traceInst, nptr->block->last);
        assert(r && "doesn't make sense!");

        std::deque<ContextGraph<Info>::GraphNodePtr> frontier;
        std::unordered_set<ContextGraph<Info>::GraphNodePtr> traversed;

        frontier.insert(frontier.end(), nptr->children.begin(), nptr->children.end());
        traversed.insert(nptr);

        while (frontier.size()) {
            auto node = frontier.front();
            frontier.pop_front();

            // Loop check
            if (traversed.count(node)) continue;
            traversed.insert(node);

            // For leaves, we interpret just to trace end
            if (node->children.empty()) {
                bool r = interpret(node, node->block->first, node->block->traceInst);
                assert(r && "doesn't make sense!");
            } else {
                bool r = interpret(node, node->block->first, node->block->last);
                redundant = r && redundant;
                frontier.insert(frontier.end(), node->children.begin(), node->children.end());
            }
        }
    }

    return redundant;
}

std::list<Instruction*> FlowAnalyzer::redundantPaths() {
    std::list<Instruction*> points;

#if 1
    errs() << "incoming debug prints\n";
    for (auto nptr : graph_.roots) {
       
        std::deque<ContextGraph<Info>::GraphNodePtr> frontier;
        std::unordered_set<ContextGraph<Info>::GraphNodePtr> traversed;

        frontier.insert(frontier.end(), 
                        nptr->children.begin(), nptr->children.end());
        traversed.insert(nptr);

        errs() << "++++++++++++++++++++++++++++\n";
        errs() << "ROOT: " << nptr.get()  << "\n" << nptr->block->str() << "\n";
        while (frontier.size()) {
            auto node = frontier.front();
            frontier.pop_front();

            // Loop check
            if (traversed.count(node)) continue;
            traversed.insert(node);

            errs() << "NODE: " << node.get() << "\n" << node->block->str() << "\n";
            // errs() << "VERDICT (parents): " << "\n";

            frontier.insert(frontier.end(), 
                            node->children.begin(), node->children.end());
        }
        errs() << "++++++++++++++++++++++++++++\n";
    }
    errs() << "Back to your regularly scheduled program\n";
#endif

    /**
     * The point here is to find the paths along which the flush is still 
     * redundant. So, we need to find the point at which, if X was the only parent,
     * would the flush be redundant? This is sort of two way, as we need to
     * have no spoiling parents and no spoiling children.
     */

    /**
     * 1. First, we iterate through top-down for the parents field.
     */
    for (auto nptr : graph_.roots) {
        assert(nptr->metadata.updated && "huh?");
        // -- Special case. For one node, it's always redundant.
        assert(!nptr->children.empty() && "not sure why we're here");

        std::deque<ContextGraph<Info>::GraphNodePtr> frontier;
        std::unordered_set<ContextGraph<Info>::GraphNodePtr> traversed;

        frontier.insert(frontier.end(), 
                        nptr->children.begin(), nptr->children.end());
        traversed.insert(nptr);

        while (frontier.size()) {
            auto node = frontier.front();
            frontier.pop_front();

            // Loop check
            if (traversed.count(node)) continue;
            traversed.insert(node);

            // For leaves, we interpret just to trace end
            for (auto parent : node->parents) {
                bool &isRedt = node->metadata.isRedtInParents;
                Info &pInfo = parent->metadata;
                // It is redundant if the parent OR grandparents redundant.
                isRedt = isRedt && (!pInfo.isNotRedundant && pInfo.isRedtInParents);
            }
            errs() << "DOWN PROP " << node.get() << " VERDICT " 
                << node->metadata.isRedtInParents << "\n";

            frontier.insert(frontier.end(), 
                            node->children.begin(), node->children.end());
        }
    }

    /**
     * 2. We need to do the back-prop part now.
     */
    for (auto nptr : graph_.leaves) {
        assert(nptr->metadata.updated && "huh?");
        // -- Special case. For one node, it's always redundant.
        assert(!nptr->parents.empty() && "not sure why we're here");

        std::deque<ContextGraph<Info>::GraphNodePtr> frontier;
        std::unordered_set<ContextGraph<Info>::GraphNodePtr> traversed;

        assert(!nptr->metadata.isNotRedundant && "???");

        frontier.insert(frontier.end(), nptr->parents.begin(), nptr->parents.end());
        traversed.insert(nptr);

        while (frontier.size()) {
            auto node = frontier.front();
            frontier.pop_front();

            // Loop check
            if (traversed.count(node)) continue;
            traversed.insert(node);

            // For leaves, we interpret just to trace end
            for (auto child : node->children) {
                bool &isRedt = node->metadata.isRedtInChildren;
                Info &cInfo = child->metadata;
                // It is redundant if the children AND grandchildren redundant.
                isRedt = isRedt && (!cInfo.isNotRedundant && cInfo.isRedtInChildren);
            }

            errs() << "UP PROP " << node.get() << " VERDICT " 
                << node->metadata.isRedtInChildren << "\n";
    
            frontier.insert(frontier.end(), 
                            node->parents.begin(), node->parents.end());
        }
    }

    /**
     * 3. Now that we have all the path info, we can find the injection points.
     * 
     * We traverse from the roots down, and when we see a node which is both
     * redundant in parents and redundant in children, we add it to the points 
     * list and stop traversing that path.
     */

    for (auto nptr : graph_.roots) {
        // If it was always provably redundant in the children, then why wouldn't
        // we go with the catch-all fix?
        assert(!nptr->metadata.isRedtInChildren && "then the original fix!");

        std::deque<ContextGraph<Info>::GraphNodePtr> frontier;
        std::unordered_set<ContextGraph<Info>::GraphNodePtr> traversed;

        frontier.insert(frontier.end(), nptr->children.begin(), nptr->children.end());
        traversed.insert(nptr);

        while (frontier.size()) {
            auto node = frontier.front();
            frontier.pop_front();

            // Loop check
            if (traversed.count(node)) continue;
            traversed.insert(node);

            if (node->metadata.isRedtInChildren && 
                node->metadata.isRedtInParents) {
                points.push_back(node->block->first);
            } else {
                frontier.insert(frontier.end(), 
                                node->children.begin(), node->children.end());
            }
        }
    }

    return points;
}

#pragma endregion