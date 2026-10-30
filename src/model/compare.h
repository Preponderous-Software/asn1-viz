// Compare mode (RFC 0034 G4): two asn1-compile/1 traces of the same schema
// (the same asn1-schema/1 type ids: two asn1c pins, or one pin with two flag
// sets), type by type. For each type: the lines its files got from each
// emitting function in A and in B, and whether the fixer treated it
// differently (its printed diff, or how many fixer events named it).
// Read-only model; no engine.
#pragma once

#include <string>
#include <vector>

#include "model/compile.h"
#include "model/schema.h"

namespace asn1viz {

struct CompareEntry {
    // Lines per function (an index into CompileCompare::functions()), with
    // the unannotated lines (includes, headers) last.
    std::vector<int> a, b;
    int linesA = 0, linesB = 0;
    int filesA = 0, filesB = 0;
    int eventsA = 0, eventsB = 0;  // fixer events attributed to it
    bool diffDiffers = false;      // the fixer's -E / -E -F diff is not the same
    bool changed() const { return a != b; }
    bool fixerDiffers() const { return diffDiffers || eventsA != eventsB; }
    int delta() const { return linesB - linesA; }
};

class CompileCompare {
public:
    // Throws unless both traces name only types of `schema` and parsed the
    // same assignments in the same order (the same schema).
    CompileCompare(const Schema& schema, CompileTrace a, CompileTrace b);

    const CompileTrace& a() const { return a_; }
    const CompileTrace& b() const { return b_; }
    // A's functions, then any only B has; unannotated is index functions().size().
    const std::vector<std::string>& functions() const { return functions_; }
    int functionIndex(const std::string& fn) const;  // -1 if none
    std::string functionName(int fn) const;          // "unannotated" past the end

    const CompareEntry& type(int id) const { return types_[static_cast<size_t>(id)]; }
    int typeCount() const { return static_cast<int>(types_.size()); }
    // The files no type owns (asn1c's standard modules' EXTERNAL.c/.h).
    const CompareEntry& shared() const { return shared_; }

    int typesChanged() const;       // types whose lines per function differ
    int typesFixerDiffers() const;  // types the fixer treated differently
    // Over every generated file, owned or not.
    int totalA() const { return a_.totalLines(); }
    int totalB() const { return b_.totalLines(); }
    // Lines per function over every file: A and B.
    std::vector<int> linesA() const;
    std::vector<int> linesB() const;

    // A trace's flags as the panel names them: the asn1c flags beyond the
    // origin lines every trace has ("default flags" when there are none).
    static std::string flagsLabel(const CompileTrace& t);

private:
    CompileTrace a_, b_;
    std::vector<std::string> functions_;
    std::vector<CompareEntry> types_;
    CompareEntry shared_;
};

}  // namespace asn1viz
