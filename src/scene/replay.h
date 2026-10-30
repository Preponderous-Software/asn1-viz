// The compilation replay's timeline (RFC 0034 section 3), without an engine.
//
// One asn1c run is a row of steps: one per parsed assignment (Parse), one per
// fixer event (Fix), one per generated file (Compile) and one per copied
// skeleton (Save). The playhead is a position in [0, steps]: the steps before
// it are done, and the one it is inside is under way (a building rising).
// Lookup events can be filtered out, which takes them off the timeline.
#pragma once

#include <string>
#include <vector>

#include "model/compile.h"

namespace asn1viz {

// The fixer's passes, grouped as the scene colours them (RFC 0034 section 3).
enum class PassFamily { Dereference, Tagging, Constraints, Values, Classes, Lookup, Other };
constexpr int kPassFamilies = 7;
PassFamily passFamily(const std::string& fn);
const char* passFamilyName(PassFamily f);

// A stage's status: "" (not recorded) and "ok" are both fine.
bool statusOk(const std::string& status);
// The fixer changed this type (a non-empty diff), and by how many printed
// lines ("+" and "-" lines of its diff).
bool fixerChanged(const CompileTrace& trace, int type);
void diffCounts(const CompileTrace& trace, int type, int& added, int& removed);

enum class Stage { Parse = 0, Fix = 1, Compile = 2, Save = 3 };
constexpr int kStages = 4;
const char* stageName(Stage s);

class Replay {
public:
    Replay(const CompileTrace& trace, int typeCount);

    const CompileTrace& trace() const { return trace_; }
    int typeCount() const { return typeCount_; }

    // The timeline.
    int steps() const { return start_[kStages]; }
    int stageStart(Stage s) const { return start_[static_cast<int>(s)]; }
    int stageSteps(Stage s) const { return start_[static_cast<int>(s) + 1] - start_[static_cast<int>(s)]; }
    Stage stage() const;            // the stage the playhead is in (Save once it reaches the end)
    int doneIn(Stage s) const;      // steps of that stage done
    float position() const { return pos_; }
    float fraction() const { return steps() > 0 ? pos_ / static_cast<float>(steps()) : 1.0f; }

    // Moving the playhead. Every move but advance() pauses.
    void seek(float position);
    void seekFraction(float f) { seek(f * static_cast<float>(steps())); }
    void stepStage(int dir);  // to the next stage's start, or back to this (or the previous) one's
    void stepEvent(int dir);  // one step
    void toEnd() { seek(static_cast<float>(steps())); }
    // Plays forward dt seconds at the current stage's rate; stops at the end.
    void advance(float dt);
    bool playing() const { return playing_; }
    void setPlaying(bool on);
    void togglePlaying() { setPlaying(!playing_); }
    float rate() const;  // steps per second in the current stage

    // Lookup events on the timeline or not (the playhead stays on the same event).
    void setLookupFiltered(bool on);
    bool lookupFiltered() const { return lookupFiltered_; }
    // The fix events on the timeline, as indexes into trace().events.
    const std::vector<int>& fixEvents() const { return fix_; }

    // What has happened by now.
    // How far a type has risen in the Parse stage, 0 (a footprint) to 1.
    float rise(int type) const;
    // The trace event the playhead last passed (-1: none yet).
    int currentEvent() const;
    // Events of the whole trace done (the unfiltered count up to the current event).
    int eventsDone() const;
    // A type whose fixer diff is not empty and whose fixing is over.
    bool diffShown(int type) const;
    // Generated files and skeletons done.
    int filesDone() const { return doneIn(Stage::Compile); }
    int skeletonsDone() const { return doneIn(Stage::Save); }
    // Pulses: types visited by the last `window` fix steps, with heat 1 for
    // the newest and the family of their newest visit.
    struct Pulse {
        int type;
        float heat;
        PassFamily family;
    };
    std::vector<Pulse> pulses(int window) const;
    // The Fix stage's pulse window, in steps, for the current rate.
    int pulseWindow() const;

private:
    void layout();

    CompileTrace trace_;
    int typeCount_ = 0;
    int start_[kStages + 1] = {0, 0, 0, 0, 0};
    std::vector<int> fix_;        // event indexes on the timeline
    std::vector<int> parseStep_;  // type -> its parse step, or -1
    std::vector<int> lastFix_;    // type -> its last position in fix_, or -1
    float pos_ = 0;
    bool playing_ = false;
    bool lookupFiltered_ = false;
};

}  // namespace asn1viz
