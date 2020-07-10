#include "BugReports.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <unistd.h>

#include "llvm/IR/Module.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace pmfix;
using namespace std;

#pragma region AddressInfo

bool AddressInfo::isSingleCacheLine(void) const {
    static uint64_t cl_sz = (uint64_t)sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    uint64_t cl_start = start() / cl_sz;
    uint64_t cl_end = end() / cl_sz;
    return cl_start == cl_end;
}

bool AddressInfo::overlaps(const AddressInfo &other) const {
    return start() < other.end() && other.start() < end();
}

bool AddressInfo::operator==(const AddressInfo &other) const {
    return address == other.address && length == other.length;
}

#pragma endregion

#pragma region LocationInfo

std::string LocationInfo::getFilename(void) const {
    size_t pos = file.find_last_of("/");
    if (pos == std::string::npos) return file;
    return file.substr(pos+1);
}

bool LocationInfo::operator==(const LocationInfo &other) const {
    if (function != other.function) return false;
    if (line != other.line) return false;

    // We want more of a partial match, since the file name strings (directories)
    // can vary. So if the shorter string fits in the longer, it's good enough.
    size_t pos = string::npos;
    if (file.size() < other.file.size()) {
        pos = other.file.find(file);
    } else {
        pos = file.find(other.file);
    }

    // if (pos != string::npos && "memset_mov_sse2_empty" == function) {
    //     errs() << "EQ: " << str() << " == " << other.str() << "\n";
    // }

    return pos != string::npos;
}

std::string LocationInfo::str() const {
    std::stringstream buffer;

    buffer << "<LocationInfo: " << function << " @ ";
    buffer << file << ":" << line << ">";

    return buffer.str();
}

#pragma endregion

#pragma region BugLocationMapper

void BugLocationMapper::insertMapping(Instruction *i) {
    // Essentially, need to get the line number and file name from the 
    // instruction debug information.
    if (!i->hasMetadata()) return;
    if (!i->getMetadata("dbg")) return;

    if (DILocation *di = dyn_cast<DILocation>(i->getMetadata("dbg"))) {
        LocationInfo li;
        li.function = i->getFunction()->getName();
        li.line = di->getLine();

        DILocalScope *ls = di->getScope();
        DIFile *df = ls->getFile();
        li.file = df->getFilename();

        // if ("memset_mov_sse2_empty" == li.function) {
        //     errs() << "DBG: " << li.str() << " ===== " << *i << '\n';
        // }

        // Now, there may already be a mapping, but it should be the previous 
        // instruction. They aren't guaranteed to be in the same basic block, 
        // but everything should be in the right function context.
        // if (locMap_.count(li)) {
        //     if (locMap_[li]->getParent() != i->getParent()) {
        //         errs() << "ERROR\n";
        //         errs() << li.str() << '\n';
        //         errs() << "Old: " << *locMap_[li] << "\n";
        //         errs() << "New: " << *i << "\n";
        //         errs() << "Context: " << *i->getFunction() << "\n";
        //     }
        //     assert(locMap_[li]->getParent() == i->getParent() && 
        //            "Assumptions violated, instructions not in same basic block!");
        // }
        locMap_[li].push_back(i);
    }
}

void BugLocationMapper::createMappings(Module &m) {
    for (Function &f : m) {
        for (BasicBlock &b : f) {
            for (Instruction &i : b) {
                // Ignore instructions we don't care too much about.
                if (!isa<StoreInst>(&i) && !isa<CallBase>(&i)) continue;
                insertMapping(&i);
            }
        }
    }

    assert(locMap_.size() && "no debug information found!!!");
}

#pragma endregion

#pragma region TraceEvent

TraceEvent::Type TraceEvent::getType(string typeString) {
    transform(typeString.begin(), typeString.end(), typeString.begin(), 
              [](unsigned char c) -> unsigned char { return std::tolower(c); });
    if (typeString == "store") return TraceEvent::STORE;
    if (typeString == "flush") return TraceEvent::FLUSH;
    if (typeString == "fence") return TraceEvent::FENCE;
    if (typeString == "assert_persisted") return TraceEvent::ASSERT_PERSISTED;
    if (typeString == "assert_ordered") return TraceEvent::ASSERT_ORDERED;
    if (typeString == "required_flush") return TraceEvent::REQUIRED_FLUSH;
    
    return TraceEvent::INVALID;
}

template< typename T >
std::string int_to_hex( T i )
{
  std::stringstream stream;
  stream << "0x" 
         << std::setfill ('0') << std::setw(sizeof(T)*2) 
         << std::hex << i;
  return stream.str();
}

std::string TraceEvent::str() const {
    std::stringstream buffer;

    buffer << "Event (time=" << timestamp << ")\n";
    buffer << "\tType: " << typeString << '\n';
    buffer << "\tLocation: " << location.str() << '\n';
    if (addresses.size()) {
        buffer << "\tAddress Info:\n";
        for (const auto &ai : addresses) {
            buffer << "\t\tAddress: " << int_to_hex(ai.address) << '\n';
            buffer << "\t\tLength: " << ai.length << '\n';
        }
    }
    buffer << "\tCall Stack:\n";
    int i = 0;
    for (const LocationInfo &li : callstack) {
        buffer << "[" << i << "] " << li.str() << '\n';
        i++;
    }

    return buffer.str();
}

bool TraceEvent::callStacksEqual(const TraceEvent &a, const TraceEvent &b) {
    if (a.callstack.size() != b.callstack.size()) {
        return false;
    }

    for (int i = 0; i < a.callstack.size(); i++) {
        const LocationInfo &la = a.callstack[i];
        const LocationInfo &lb = b.callstack[i];
        errs() << "\t\t" << la.str() << " ?= " << lb.str() << '\n';
        if (la.function != lb.function) return false;
        if (la.file != lb.file) return false;
        if (i > 0 && la.line != lb.line) return false;
    }

    return true;
}

#pragma endregion

#pragma region TraceInfo

void TraceInfo::addEvent(TraceEvent &&event) {
    if (event.isBug) {
        bugs_.push_back(events_.size());
    }

    events_.emplace_back(event); 
}

std::string TraceInfo::str(void) const {
    std::stringstream buffer;

    for (const auto &event : events_) {
        buffer << event.str() << '\n';
    }

    return buffer.str();
}

#pragma endregion

#pragma region TraceInfoBuilder

void TraceInfoBuilder::processEvent(TraceInfo &ti, YAML::Node event) {
    TraceEvent e;
    e.typeString = event["event"].as<string>();

    TraceEvent::Type event_type = TraceEvent::getType(e.typeString);

    assert(event_type != TraceEvent::INVALID);
    
    e.type = event_type;
    e.timestamp = event["timestamp"].as<uint64_t>();
    e.location.function = event["function"].as<string>();
    e.location.file = event["file"].as<string>();
    e.location.line = event["line"].as<int64_t>();
    e.isBug = event["is_bug"].as<bool>();

    assert(event["stack"].IsSequence() && "Don't know what to do!");
    for (size_t i = 0; i < event["stack"].size(); ++i) {
        YAML::Node sf = event["stack"][i];
        LocationInfo li;
        li.function = sf["function"].as<string>();
        li.file = sf["file"].as<string>();
        li.line = sf["line"].as<int64_t>();
        e.callstack.emplace_back(li);
    }

    switch (e.type) {
        case TraceEvent::STORE:
        case TraceEvent::FLUSH:
        case TraceEvent::ASSERT_PERSISTED:
        case TraceEvent::REQUIRED_FLUSH:
            AddressInfo ai;
            ai.address = event["address"].as<uint64_t>();
            ai.length = event["length"].as<uint64_t>();
            e.addresses.push_back(ai);
            break;
        case TraceEvent::ASSERT_ORDERED:
            AddressInfo a, b;
            a.address = event["address_a"].as<uint64_t>();
            a.length = event["length_a"].as<uint64_t>();
            b.address = event["address_b"].as<uint64_t>();
            b.length = event["length_b"].as<uint64_t>();
            e.addresses.push_back(a);
            e.addresses.push_back(b);
            break;
        default:
            break;
    }

    ti.addEvent(std::move(e));
}

TraceInfo TraceInfoBuilder::build(void) {
    TraceInfo ti(doc_["metadata"]);

    auto trace = doc_["trace"];
    assert(trace.IsSequence() && "Don't know what to do otherwise!");
    for (size_t i = 0; i < trace.size(); ++i) {
        processEvent(ti, trace[i]);
    }

    return ti;
}

#pragma endregion