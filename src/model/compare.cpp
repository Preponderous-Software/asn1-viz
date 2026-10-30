#include "model/compare.h"

#include <stdexcept>
#include <utility>

namespace asn1viz {

namespace {

void addFile(CompareEntry& e, const CompileCompare& c, const CompileFile& f, bool inA) {
    std::vector<int>& lines = inA ? e.a : e.b;
    for (const auto& [fn, n] : f.byFn) lines[static_cast<size_t>(c.functionIndex(fn))] += n;
    lines.back() += f.unannotated;
    (inA ? e.linesA : e.linesB) += f.lines;
    ++(inA ? e.filesA : e.filesB);
}

}  // namespace

CompileCompare::CompileCompare(const Schema& schema, CompileTrace a, CompileTrace b) : a_(std::move(a)), b_(std::move(b)) {
    if (!a_.matches(schema) || !b_.matches(schema))
        throw std::runtime_error("compare: a trace names a type that is not in the schema");
    bool same = a_.parse.size() == b_.parse.size();
    for (size_t i = 0; same && i < a_.parse.size(); ++i)
        same = a_.parse[i].type == b_.parse[i].type && a_.parse[i].line == b_.parse[i].line;
    if (!same) throw std::runtime_error("compare: the two traces did not parse the same schema");

    functions_ = a_.functions;
    auto addFns = [&](const CompileTrace& t) {
        for (const std::string& fn : t.functions)
            if (functionIndex(fn) < 0) functions_.push_back(fn);
        for (const CompileFile& f : t.files)
            for (const auto& kv : f.byFn)
                if (functionIndex(kv.first) < 0) functions_.push_back(kv.first);
    };
    addFns(a_);
    addFns(b_);

    const size_t n = schema.types.size(), slots = functions_.size() + 1;
    CompareEntry blank;
    blank.a.assign(slots, 0);
    blank.b.assign(slots, 0);
    types_.assign(n, blank);
    shared_ = blank;
    for (int pass = 0; pass < 2; ++pass) {
        const CompileTrace& t = pass == 0 ? a_ : b_;
        for (const CompileFile& f : t.files)
            addFile(f.type >= 0 ? types_[static_cast<size_t>(f.type)] : shared_, *this, f, pass == 0);
        for (const CompileEvent& e : t.events)
            if (e.type >= 0) ++(pass == 0 ? types_[static_cast<size_t>(e.type)].eventsA : types_[static_cast<size_t>(e.type)].eventsB);
    }
    for (size_t i = 0; i < n; ++i) {
        const auto da = a_.diffs.find(static_cast<int>(i)), db = b_.diffs.find(static_cast<int>(i));
        const std::string sa = da == a_.diffs.end() ? "" : da->second, sb = db == b_.diffs.end() ? "" : db->second;
        types_[i].diffDiffers = sa != sb;
    }
}

int CompileCompare::functionIndex(const std::string& fn) const {
    for (size_t i = 0; i < functions_.size(); ++i)
        if (functions_[i] == fn) return static_cast<int>(i);
    return -1;
}

std::string CompileCompare::functionName(int fn) const {
    return fn >= 0 && fn < static_cast<int>(functions_.size()) ? functions_[static_cast<size_t>(fn)] : "unannotated";
}

int CompileCompare::typesChanged() const {
    int k = 0;
    for (const CompareEntry& e : types_) k += e.changed() ? 1 : 0;
    return k;
}

int CompileCompare::typesFixerDiffers() const {
    int k = 0;
    for (const CompareEntry& e : types_) k += e.fixerDiffers() ? 1 : 0;
    return k;
}

std::vector<int> CompileCompare::linesA() const {
    std::vector<int> out(functions_.size() + 1, 0);
    for (const CompareEntry& e : types_)
        for (size_t i = 0; i < out.size(); ++i) out[i] += e.a[i];
    for (size_t i = 0; i < out.size(); ++i) out[i] += shared_.a[i];
    return out;
}

std::vector<int> CompileCompare::linesB() const {
    std::vector<int> out(functions_.size() + 1, 0);
    for (const CompareEntry& e : types_)
        for (size_t i = 0; i < out.size(); ++i) out[i] += e.b[i];
    for (size_t i = 0; i < out.size(); ++i) out[i] += shared_.b[i];
    return out;
}

std::string CompileCompare::flagsLabel(const CompileTrace& t) {
    std::string s;
    for (const std::string& f : t.flags)
        if (f != "-debug-output-origin-lines") s += (s.empty() ? "" : " ") + f;
    return s.empty() ? "default flags" : s;
}

}  // namespace asn1viz
