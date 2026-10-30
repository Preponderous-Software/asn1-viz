// The compilation replay (RFC 0034 K1) on datasets/demo/compile.json (the K0 trace of the demo module):
// the asn1-compile/1 reader, the timeline (stages, stepping, seeking,
// playback, the lookup filter), what has happened by a playhead position
// (rise, pulses, diff marks, floors), and the geometry's tags.
#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "check.h"
#include "model/compile.h"
#include "model/schema.h"
#include "scene/compile_city.h"
#include "scene/replay.h"

using namespace asn1viz;

namespace {

bool throwsOn(const std::string& text) {
    try {
        loadCompileTrace(json::parse(text));
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

bool matchesDemo(const std::string& text, const Schema& s) { return loadCompileTrace(json::parse(text)).matches(s); }

void checkReader(const CompileTrace& t, const Schema& s) {
    CHECK(!t.parse.empty());
    CHECK(!t.events.empty());
    CHECK(!t.files.empty());
    CHECK(!t.skeletons.empty());
    CHECK(statusOk(t.fixStatus) && statusOk(t.compileStatus));
    CHECK(t.matches(s));
    CHECK(t.schema == "schema.json");
    // Every type the trace names is one of the schema's; null is unattributed.
    bool inRange = true, someUnattributed = false;
    for (const CompileEvent& e : t.events) {
        inRange = inRange && e.type < static_cast<int>(s.types.size());
        someUnattributed = someUnattributed || e.type < 0;
    }
    CHECK(inRange);
    CHECK(someUnattributed);
    // Each file's lines are its functions' plus the unannotated ones.
    bool sums = true;
    for (const CompileFile& f : t.files) {
        int n = f.unannotated;
        for (const auto& kv : f.byFn) {
            n += kv.second;
            sums = sums && std::find(t.functions.begin(), t.functions.end(), kv.first) != t.functions.end();
        }
        sums = sums && n == f.lines;
    }
    CHECK(sums);
    const int msg = s.typeByName("Msg");
    CHECK(msg >= 0 && fixerChanged(t, msg));
    int add = 0, del = 0;
    diffCounts(t, msg, add, del);
    CHECK(add >= 1 && del >= 1);

    CHECK(passFamily("asn1f_fix_dereference_types") == PassFamily::Dereference);
    CHECK(passFamily("asn1f_fix_constr_autotag") == PassFamily::Tagging);
    CHECK(passFamily("asn1f_resolve_constraints") == PassFamily::Constraints);
    CHECK(passFamily("asn1f_check_constraints") == PassFamily::Constraints);
    CHECK(passFamily("asn1f_fix_integer") == PassFamily::Values);
    CHECK(passFamily("asn1f_parse_class_object") == PassFamily::Classes);
    CHECK(passFamily("asn1f_lookup_symbol_impl") == PassFamily::Lookup);
    CHECK(passFamily("asn1f_find_terminal_thing") == PassFamily::Lookup);
    CHECK(passFamily("asn1f_fix_constr_ext") == PassFamily::Other);

    // Not this format, or a type outside the schema.
    CHECK(throwsOn("{\"format\":\"asn1-schema/1\"}"));
    CHECK(!matchesDemo("{\"format\":\"asn1-compile/1\",\"parse\":[{\"type\":700,\"line\":1}]}", s));
    CHECK(matchesDemo("{\"format\":\"asn1-compile/1\",\"fix\":{\"events\":[{\"seq\":0,\"fn\":\"f\",\"type\":null}]}}", s));
}

void checkReplay(const CompileTrace& t, int types, int msg) {
    Replay r(t, types);
    const int P = static_cast<int>(t.parse.size()), E = static_cast<int>(t.events.size()),
              F = static_cast<int>(t.files.size()), K = static_cast<int>(t.skeletons.size());
    CHECK(r.steps() == P + E + F + K);
    CHECK(r.stage() == Stage::Parse && r.position() == 0 && !r.playing());
    for (int ty = 0; ty < types; ++ty) CHECK(r.rise(ty) == 0);

    // Steps and stages.
    r.stepEvent(1);
    CHECK(r.position() == 1 && r.doneIn(Stage::Parse) == 1);
    CHECK(r.rise(t.parse[0].type) == 1);
    r.stepStage(1);
    CHECK(r.stage() == Stage::Fix && r.position() == static_cast<float>(P) && r.currentEvent() == -1);
    for (int ty = 0; ty < types; ++ty) CHECK(r.rise(ty) == 1);
    r.stepEvent(1);
    CHECK(r.currentEvent() == 0 && r.eventsDone() == 1);
    r.stepStage(-1);  // back to the start of Fix
    CHECK(r.position() == static_cast<float>(P));
    r.stepStage(-1);  // and from there to Parse
    CHECK(r.position() == 0);
    r.stepStage(1);
    r.stepStage(1);
    CHECK(r.stage() == Stage::Compile && r.filesDone() == 0 && r.eventsDone() == E);
    r.stepStage(1);
    CHECK(r.stage() == Stage::Save && r.filesDone() == F);
    r.stepStage(1);
    CHECK(r.position() == static_cast<float>(r.steps()) && r.skeletonsDone() == K && r.stage() == Stage::Save);
    r.stepEvent(1);
    CHECK(r.position() == static_cast<float>(r.steps()));
    r.seekFraction(-3);
    CHECK(r.position() == 0);

    // Playback runs at the stage's rate and stops at the end.
    r.setPlaying(true);
    CHECK(r.playing());
    r.advance(0.25f);
    CHECK(r.position() > 0 && r.position() <= r.rate() * 0.25f + 1e-3f);
    for (int i = 0; i < 2000 && r.playing(); ++i) r.advance(0.1f);
    CHECK(!r.playing() && r.position() == static_cast<float>(r.steps()));
    r.setPlaying(true);  // from the end: from the start again
    CHECK(r.playing() && r.position() == 0);
    r.stepEvent(1);
    CHECK(!r.playing());

    // Diff marks show once a changed type's last fixer event has passed.
    CHECK(fixerChanged(t, msg));
    r.seek(static_cast<float>(P));
    CHECK(!r.diffShown(msg));
    int last = -1;
    for (size_t j = 0; j < r.fixEvents().size(); ++j)
        if (t.events[static_cast<size_t>(r.fixEvents()[j])].type == msg) last = static_cast<int>(j);
    CHECK(last >= 0);
    r.seek(static_cast<float>(P + last));
    CHECK(!r.diffShown(msg));
    r.seek(static_cast<float>(P + last + 1));
    CHECK(r.diffShown(msg));

    // Pulses: the newest visit is hottest, and only inside the Fix stage.
    r.seek(static_cast<float>(P + 3));
    const auto pulses = r.pulses(4);
    CHECK(!pulses.empty());
    const CompileEvent& now = t.events[static_cast<size_t>(r.currentEvent())];
    if (now.type >= 0) CHECK(pulses.front().type == now.type && pulses.front().heat == 1.0f);
    r.seek(0);
    CHECK(r.pulses(4).empty());

    // The lookup filter takes lookups off the timeline and keeps the playhead's event.
    int lookups = 0;
    for (const CompileEvent& e : t.events) lookups += passFamily(e.fn) == PassFamily::Lookup ? 1 : 0;
    CHECK(lookups > 0);
    int target = -1;
    for (int i = 0; i < E; ++i)
        if (passFamily(t.events[static_cast<size_t>(i)].fn) != PassFamily::Lookup && i > 2) {
            target = i;
            break;
        }
    r.seek(static_cast<float>(P + target + 1));
    CHECK(r.currentEvent() == target);
    r.setLookupFiltered(true);
    CHECK(r.stageSteps(Stage::Fix) == E - lookups);
    CHECK(r.steps() == P + E - lookups + F + K);
    CHECK(r.currentEvent() == target);
    bool none = true;
    for (int e : r.fixEvents()) none = none && passFamily(t.events[static_cast<size_t>(e)].fn) != PassFamily::Lookup;
    CHECK(none);
    r.setLookupFiltered(false);
    CHECK(r.currentEvent() == target && r.stageSteps(Stage::Fix) == E);
}

void checkGeometry(const Schema& s, const CompileTrace& t) {
    CompileCity c(s, t, CityStyle{});
    Replay& r = c.replay();
    const int types = static_cast<int>(s.types.size());

    // Tags round-trip.
    CHECK(decodeTag(-1).kind == CompilePick::None);
    CHECK(decodeTag(3).kind == CompilePick::Type && decodeTag(3).type == 3);
    const CompilePick b = decodeTag(bandTag(1, 5));
    CHECK(b.kind == CompilePick::Band && b.type == 1 && b.fn == 5);
    const CompilePick k = decodeTag(skeletonTag(42));
    CHECK(k.kind == CompilePick::Skeleton && k.skeleton == 42);
    const CompilePick sh = decodeTag(sharedTag(2, 7));
    CHECK(sh.kind == CompilePick::SharedBand && sh.file == 2 && sh.fn == 7);

    // At the start: every building is a footprint, and the library is empty.
    const long long key0 = c.geometryKey();
    for (int ty = 0; ty < types; ++ty) CHECK(c.height(ty) < 0.05f);
    MeshBuilder lib0;
    c.buildLibrary(lib0);
    for (const TriTag& tag : lib0.tags()) CHECK(tag.tag == -1);  // only the shelves' ground

    // After parsing: city heights; the geometry key moved.
    r.seek(static_cast<float>(r.stageStart(Stage::Fix)));
    CHECK(c.geometryKey() != key0);
    for (int ty = 0; ty < types; ++ty) CHECK(std::fabs(c.height(ty) - c.city().lot(ty).height) < 1e-4f);
    const long long keyFix = c.geometryKey();
    r.stepEvent(1);
    CHECK(c.geometryKey() == keyFix);  // fixer events only change the marks

    // At the end: each type with files is a stack of bands, one per function
    // that wrote lines for it, as tall as its lines.
    r.toEnd();
    std::set<int> fns;
    for (size_t d = 0; d < c.city().districts().size(); ++d) {
        MeshBuilder m;
        c.buildDistrict(d, m);
        for (const TriTag& tag : m.tags()) {
            const CompilePick p = decodeTag(tag.tag);
            if (p.kind == CompilePick::Band) {
                CHECK(p.type >= 0 && p.type < types);
                CHECK(c.emitted(p.type)[static_cast<size_t>(p.fn)] > 0);
                fns.insert(p.fn);
            }
        }
    }
    CHECK(!fns.empty());
    const int msg = s.typeByName("Msg");
    CHECK(c.emittedTotal(msg) > 0);
    CHECK(std::fabs(c.height(msg) - static_cast<float>(c.emittedTotal(msg)) * c.lineScale()) < 1e-3f);
    // Every skeleton is on its shelf, each tagged.
    MeshBuilder lib;
    c.buildLibrary(lib);
    std::set<int> skeletons;
    for (const TriTag& tag : lib.tags())
        if (decodeTag(tag.tag).kind == CompilePick::Skeleton) skeletons.insert(decodeTag(tag.tag).skeleton);
    CHECK(skeletons.size() == t.skeletons.size());
    // The shelves stand clear of the city.
    for (const District& d : c.city().districts()) CHECK(c.skeletonShelf().x0 > d.x1);
    // The home camera sees the city and the library.
    const CameraMatrices cm = matrices(c.homeCamera(16.0f / 9.0f), 16.0f / 9.0f);
    float x, y;
    CHECK(toScreen(cm, c.skeletonTop(0), 1280, 720, x, y));
    CHECK(toScreen(cm, c.city().districtCentre(0), 1280, 720, x, y));

    // Marks: a diff mark on Msg at the end, tagged with Msg.
    MeshBuilder marks;
    c.buildMarks(CompilePick{}, marks);
    bool msgMarked = false;
    for (const TriTag& tag : marks.tags()) msgMarked = msgMarked || tag.tag == msg;
    CHECK(msgMarked);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const Schema s = loadSchemaFile(root + "/datasets/demo/schema.json");
    const CompileTrace t = loadCompileTraceFile(root + "/datasets/demo/compile.json");
    checkReader(t, s);
    checkReplay(t, static_cast<int>(s.types.size()), s.typeByName("Msg"));
    checkGeometry(s, t);
    TEST_MAIN_END();
}
