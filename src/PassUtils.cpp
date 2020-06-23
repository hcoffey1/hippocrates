#include "PassUtils.hpp"

using namespace pmfix;

bool utils::checkInlineAsmEq(const Instruction *iptr...) {
    va_list args;
    va_start(args, iptr);

    const InlineAsm *ia = nullptr;
    const Instruction &i = *iptr;
    if (i.getOpcode() == Instruction::Call) {
        const CallInst &ci = static_cast<const CallInst&>(i);
        if (ci.isInlineAsm()) {
            ia = static_cast<InlineAsm*>(ci.getCalledValue());
        }
    }

    char *asmStr;
    while(nullptr != ia &&
          nullptr != (asmStr = va_arg(args, char*))) {
        if (ia->getAsmString() == asmStr) {
            va_end(args);
            return true;
        }
    }

    va_end(args);
    return false;
}

bool utils::checkInstrinicInst(const Instruction *iptr...) {
    va_list args;
    va_start(args, iptr);

    const Function *fn = nullptr;
    const Instruction &i = *iptr;
    if (i.getOpcode() == Instruction::Call) {
        const CallInst &ci = static_cast<const CallInst&>(i);
        if (ci.getIntrinsicID() != Intrinsic::not_intrinsic) {
            const IntrinsicInst &ii = static_cast<const IntrinsicInst&>(i);
            fn = ii.getCalledFunction();
        }
    }

    char *fnName;
    while(nullptr != fn &&
          nullptr != (fnName = va_arg(args, char*))) {
        if (fn->getName().contains(fnName)) {
            va_end(args);
            return true;
        }
    }

    va_end(args);
    return false;
}

bool utils::isFlush(const Instruction &i) {
    return checkInstrinicInst(&i,
                              "clflush",
                              nullptr) ||
           utils::checkInlineAsmEq(&i,
                   ".byte 0x66; clflush $0",
                   ".byte 0x66; xsaveopt $0", nullptr);
}

bool utils::isFence(const Instruction &i) {
    return checkInstrinicInst(&i, "sfence", nullptr);
}

Value* utils::getPtrLoc(Value *v) {
    if (isa<User>(v)) {
        User *u = dyn_cast<User>(v);
        for (auto &op : u->operands()) {
            if (isa<AllocaInst>(op)) {
                return op;
            }
        }
    }

    return v;
}

unordered_set<const Value*> utils::getPtrsFromStoredLocs(const Value *ptr) {
    unordered_set<const Value*> ptrs;

    for (const auto *u : ptr->users()) {
        const StoreInst *si = dyn_cast<StoreInst>(u);
        if (nullptr != si
            && ptr == si->getValueOperand()) {
            errs() << "\t\t(store): " << *si << "\n";
            const Value *store_loc = si->getPointerOperand();

            for (const auto *su : store_loc->users()) {
                //errs() << "\t\t\t(store user): " << *su << "\n";
                const LoadInst *li = dyn_cast<LoadInst>(su);
                if (nullptr != li
                    && store_loc == li->getPointerOperand()) {
                    errs() << "\t\t\t(load): " << *li << "\n";

                    ptrs.insert(li);
                }
            }
        }
    }

    return ptrs;
}

void utils::getDerivativePtrs(unordered_set<const Value*> &s)
{
    if (s.empty()) return;
    unordered_set<const Value*> der;

    for (auto *v : s) {
        // We're not interested with dereferenced values from NVM.
        if (!v->getType()->isPointerTy()) continue;

        for (auto *u : v->users()) {
            if (u->getType()->isPointerTy()) {
                der.insert(u);
            }
        }

        auto fromStores = getPtrsFromStoredLocs(v);
        der.insert(fromStores.begin(), fromStores.end());
    }

    getDerivativePtrs(der);

    s.insert(der.begin(), der.end());
}

void utils::getModifiers(const Value* ptr, unordered_set<const Value*> &s) {
    for (auto *u : ptr->users()) {
        if (const StoreInst *si = dyn_cast<StoreInst>(u)) {
            if (si->getPointerOperand() == ptr) {
                s.insert(u);
            }
        }

        if (isFlush(*dyn_cast<Instruction>(u))) {
            s.insert(u);
        }
    }
}

list<const Function*> utils::getNestedFunctionCalls(const BasicBlock *bb) {
    list<const Function*> fns;

    for (const Instruction &i : *bb) {
        const CallInst *ci = dyn_cast<CallInst>(&i);
        if (ci && !ci->isInlineAsm()) {
            const Function *cfn = ci->getCalledFunction();
            if (cfn && !cfn->isIntrinsic()) {
                fns.push_back(cfn);
            }
        }
    }

    return fns;
}