#include "scene/replay.h"

#include <algorithm>
#include <cmath>

namespace asn1viz {

namespace {

// Each stage plays in about this many seconds, but never slower than the
// slowest rate (a small schema) nor faster than the fastest (a frame can
// only show so much).
constexpr float kSeconds[kStages] = {5, 24, 10, 4};
constexpr float kSlowest[kStages] = {2, 6, 3, 12};
constexpr float kFastest[kStages] = {80, 600, 60, 200};

bool has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

}  // namespace

PassFamily passFamily(const std::string& fn) {
    // Symbol lookups are heavy traffic, not edits: their own family, so the
    // view can filter them out (RFC 0034, Risks).
    if (has(fn, "lookup") || has(fn, "find_terminal") || has(fn, "find_")) return PassFamily::Lookup;
    if (has(fn, "dereference")) return PassFamily::Dereference;
    if (has(fn, "constraint")) return PassFamily::Constraints;
    if (has(fn, "tag")) return PassFamily::Tagging;
    if (has(fn, "class") || has(fn, "object") || has(fn, "param")) return PassFamily::Classes;
    if (has(fn, "value") || has(fn, "integer") || has(fn, "enum") || has(fn, "bit_string") || has(fn, "bstring") ||
        has(fn, "cstring") || has(fn, "default"))
        return PassFamily::Values;
    return PassFamily::Other;
}

const char* passFamilyName(PassFamily f) {
    switch (f) {
        case PassFamily::Dereference: return "dereference";
        case PassFamily::Tagging: return "tagging";
        case PassFamily::Constraints: return "constraints";
        case PassFamily::Values: return "values";
        case PassFamily::Classes: return "classes";
        case PassFamily::Lookup: return "lookup";
        case PassFamily::Other: return "structure, other";
    }
    return "other";
}

bool statusOk(const std::string& status) { return status.empty() || status == "ok"; }

bool fixerChanged(const CompileTrace& trace, int type) {
    const auto it = trace.diffs.find(type);
    return it != trace.diffs.end() && !it->second.empty();
}

void diffCounts(const CompileTrace& trace, int type, int& added, int& removed) {
    added = removed = 0;
    const auto it = trace.diffs.find(type);
    if (it == trace.diffs.end()) return;
    const std::string& d = it->second;
    size_t at = 0;
    while (at < d.size()) {
        const size_t end = d.find('\n', at);
        // "+++"/"---" headers, if a diff carries them, are not lines.
        const bool header = d.compare(at, 3, "+++") == 0 || d.compare(at, 3, "---") == 0;
        if (!header && d[at] == '+') ++added;
        if (!header && d[at] == '-') ++removed;
        if (end == std::string::npos) break;
        at = end + 1;
    }
}

const char* stageName(Stage s) {
    switch (s) {
        case Stage::Parse: return "Parse";
        case Stage::Fix: return "Fix";
        case Stage::Compile: return "Compile";
        case Stage::Save: return "Save";
    }
    return "?";
}

Replay::Replay(const CompileTrace& trace, int typeCount) : trace_(trace), typeCount_(std::max(0, typeCount)) {
    parseStep_.assign(static_cast<size_t>(typeCount_), -1);
    for (size_t i = 0; i < trace_.parse.size(); ++i) {
        const int t = trace_.parse[i].type;
        if (t >= 0 && t < typeCount_ && parseStep_[static_cast<size_t>(t)] < 0)
            parseStep_[static_cast<size_t>(t)] = static_cast<int>(i);
    }
    layout();
}

void Replay::layout() {
    fix_.clear();
    for (size_t i = 0; i < trace_.events.size(); ++i)
        if (!lookupFiltered_ || passFamily(trace_.events[i].fn) != PassFamily::Lookup) fix_.push_back(static_cast<int>(i));
    lastFix_.assign(static_cast<size_t>(typeCount_), -1);
    for (size_t j = 0; j < fix_.size(); ++j) {
        const int t = trace_.events[static_cast<size_t>(fix_[j])].type;
        if (t >= 0 && t < typeCount_) lastFix_[static_cast<size_t>(t)] = static_cast<int>(j);
    }
    start_[0] = 0;
    start_[1] = static_cast<int>(trace_.parse.size());
    start_[2] = start_[1] + static_cast<int>(fix_.size());
    start_[3] = start_[2] + static_cast<int>(trace_.files.size());
    start_[4] = start_[3] + static_cast<int>(trace_.skeletons.size());
}

Stage Replay::stage() const {
    // The last non-empty stage the playhead has reached.
    for (int s = kStages - 1; s >= 0; --s)
        if (start_[s + 1] > start_[s] && pos_ >= static_cast<float>(start_[s])) return static_cast<Stage>(s);
    return Stage::Parse;
}

int Replay::doneIn(Stage s) const {
    const int a = start_[static_cast<int>(s)], b = start_[static_cast<int>(s) + 1];
    const int done = static_cast<int>(std::floor(pos_ + 1e-4f));
    return std::clamp(done - a, 0, b - a);
}

void Replay::seek(float p) {
    pos_ = std::clamp(p, 0.0f, static_cast<float>(steps()));
    playing_ = false;
}

void Replay::setPlaying(bool on) {
    // Play from the start again once the end is reached.
    if (on && pos_ >= static_cast<float>(steps())) pos_ = 0;
    playing_ = on && steps() > 0;
}

void Replay::stepStage(int dir) {
    const int cur = static_cast<int>(stage());
    if (dir > 0) {
        int s = cur + 1;
        while (s < kStages && start_[s + 1] == start_[s]) ++s;  // skip empty stages
        seek(static_cast<float>(s < kStages ? start_[s] : steps()));
        return;
    }
    // Back to this stage's start, or, from there, to the previous non-empty one's.
    if (pos_ > static_cast<float>(start_[cur]) + 1e-4f) {
        seek(static_cast<float>(start_[cur]));
        return;
    }
    int s = cur - 1;
    while (s > 0 && start_[s + 1] == start_[s]) --s;
    seek(static_cast<float>(start_[std::max(0, s)]));
}

void Replay::stepEvent(int dir) {
    const float f = std::floor(pos_ + 1e-4f);
    seek(dir > 0 ? f + 1 : (std::fabs(pos_ - f) < 1e-4f ? f - 1 : f));
}

float Replay::rate() const {
    const int s = static_cast<int>(stage());
    const float n = static_cast<float>(start_[s + 1] - start_[s]);
    return std::clamp(n / kSeconds[s], kSlowest[s], kFastest[s]);
}

void Replay::advance(float dt) {
    if (!playing_ || dt <= 0) return;
    pos_ = std::min(static_cast<float>(steps()), pos_ + rate() * dt);
    if (pos_ >= static_cast<float>(steps())) playing_ = false;
}

void Replay::setLookupFiltered(bool on) {
    if (on == lookupFiltered_) return;
    // Keep the playhead on the same event (or the one after a hidden one),
    // and at the same place in the other stages.
    const Stage s = stage();
    const float into = pos_ - static_cast<float>(start_[static_cast<int>(s)]);
    const int ev = currentEvent();
    lookupFiltered_ = on;
    layout();
    if (s == Stage::Fix) {
        int j = 0;
        while (j < static_cast<int>(fix_.size()) && fix_[static_cast<size_t>(j)] <= ev) ++j;
        pos_ = static_cast<float>(start_[1] + j);
    } else {
        pos_ = std::min(static_cast<float>(steps()), static_cast<float>(start_[static_cast<int>(s)]) + into);
    }
}

float Replay::rise(int type) const {
    if (type < 0 || type >= typeCount_) return 1;
    const int k = parseStep_[static_cast<size_t>(type)];
    // A type the parse list leaves out stands up when parsing ends.
    const float at = static_cast<float>(k >= 0 ? k : start_[1] - 1);
    return std::clamp(pos_ - at, 0.0f, 1.0f);
}

int Replay::currentEvent() const {
    const int done = doneIn(Stage::Fix);
    return done > 0 ? fix_[static_cast<size_t>(done - 1)] : -1;
}

int Replay::eventsDone() const {
    if (pos_ >= static_cast<float>(start_[2])) return static_cast<int>(trace_.events.size());
    return currentEvent() + 1;
}

bool Replay::diffShown(int type) const {
    if (!fixerChanged(trace_, type)) return false;
    if (pos_ >= static_cast<float>(start_[2])) return true;
    const int last = type >= 0 && type < typeCount_ ? lastFix_[static_cast<size_t>(type)] : -1;
    return last >= 0 && doneIn(Stage::Fix) > last;
}

int Replay::pulseWindow() const {
    // About 0.6 s of events at the Fix stage's rate.
    const float n = static_cast<float>(stageSteps(Stage::Fix));
    const float r = std::clamp(n / kSeconds[1], kSlowest[1], kFastest[1]);
    return std::max(3, static_cast<int>(r * 0.6f));
}

std::vector<Replay::Pulse> Replay::pulses(int window) const {
    std::vector<Pulse> out;
    if (stage() != Stage::Fix || window < 1) return out;
    const int done = doneIn(Stage::Fix);
    std::vector<char> seen(static_cast<size_t>(typeCount_), 0);
    for (int j = done - 1; j >= 0 && j >= done - window; --j) {
        const CompileEvent& e = trace_.events[static_cast<size_t>(fix_[static_cast<size_t>(j)])];
        if (e.type < 0 || e.type >= typeCount_ || seen[static_cast<size_t>(e.type)]) continue;
        seen[static_cast<size_t>(e.type)] = 1;
        const float age = static_cast<float>(done - 1 - j) / static_cast<float>(window);
        out.push_back({e.type, 1.0f - age, passFamily(e.fn)});
    }
    return out;
}

}  // namespace asn1viz
