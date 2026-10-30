#include "view/compile_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace asn1viz {

namespace {

// SDL3 scancodes.
constexpr int32_t kScanSpace = 44, kScanLeftBracket = 47, kScanRightBracket = 48, kScanComma = 54,
                  kScanPeriod = 55, kScanL = 15, kScanF = 9, kScanC = 6, kScanEnd = 77;

const float kInk[4] = {0.92f, 0.94f, 0.97f, 1};
const float kDim[4] = {0.62f, 0.66f, 0.74f, 1};
const float kGold[4] = {1, 0.82f, 0.25f, 1};
const float kMark[4] = {1, 0.3f, 0.6f, 1};
const float kFail[4] = {1, 0.35f, 0.35f, 1};
const float kStageInk[kStages][4] = {
    {0.6f, 0.8f, 1, 1}, {1, 0.62f, 0.28f, 1}, {0.5f, 0.95f, 0.6f, 1}, {0.9f, 0.75f, 0.5f, 1}};

std::string commas(long long n) {
    std::string s = std::to_string(n < 0 ? -n : n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return n < 0 ? "-" + s : s;
}

void rgba(const Rgb& c, float out[4]) {
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
    out[3] = 1;
}

struct Rect {
    float x0, y0, x1, y1;
};

bool overlaps(const std::vector<Rect>& taken, const Rect& r) {
    for (const Rect& t : taken)
        if (r.x0 < t.x1 && r.x1 > t.x0 && r.y0 < t.y1 && r.y1 > t.y0) return true;
    return false;
}

// The first changed line of a diff, without its +/-.
std::string firstChange(const std::string& diff, char sign) {
    size_t at = 0;
    while (at < diff.size()) {
        const size_t end = diff.find('\n', at);
        const std::string line = diff.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!line.empty() && line[0] == sign && line.compare(0, 3, std::string(3, sign)) != 0) {
            size_t b = 1;
            while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
            return line.substr(b);
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return "";
}

}  // namespace

CompileView::CompileView(const Schema& schema, const CompileTrace& trace, CityStyle style)
    : city_(schema, trace, style) {}

void CompileView::upload(strata::Engine& engine) {
    districtIds_.clear();
    libraryId_ = marksId_ = 0;
    builtKey_ = -1;
    rebuild(engine, true);
}

void CompileView::rebuild(strata::Engine& engine, bool geometry) {
    if (geometry) {
        for (uint32_t id : districtIds_)
            if (id) engine.removeMesh(id);
        districtIds_.clear();
        for (size_t d = 0; d < city_.city().districts().size(); ++d) {
            MeshBuilder m;
            city_.buildDistrict(d, m);
            districtIds_.push_back(uploadMesh(engine, m));
        }
        if (libraryId_) engine.removeMesh(libraryId_);
        MeshBuilder lib;
        city_.buildLibrary(lib);
        libraryId_ = uploadMesh(engine, lib);
        builtKey_ = city_.geometryKey();
    }
    if (marksId_) engine.removeMesh(marksId_);
    MeshBuilder marks;
    city_.buildMarks(selected_, marks);
    marksId_ = uploadMesh(engine, marks, true);
    marksDirty_ = false;
}

OrbitCamera CompileView::home(float aspect) const { return city_.homeCamera(aspect); }

void CompileView::select(const CompilePick& p) {
    selected_ = p;
    marksDirty_ = true;
}

bool CompileView::selectName(const std::string& name) {
    const size_t slash = name.find('/');
    const int t = city_.schema().typeByName(name.substr(0, slash));
    if (t < 0) return false;
    CompilePick p;
    p.kind = CompilePick::Type;
    p.type = t;
    if (slash != std::string::npos) {
        p.fn = city_.functionIndex(name.substr(slash + 1));
        if (p.fn < 0) return false;
        p.kind = CompilePick::Band;
    }
    select(p);
    return true;
}

std::string CompileView::selectionName() const {
    const CompileTrace& t = city_.trace();
    auto fnName = [&](int fn) {
        return fn >= 0 && fn < static_cast<int>(city_.functions().size()) ? city_.functions()[static_cast<size_t>(fn)]
                                                                     : std::string("unannotated");
    };
    switch (selected_.kind) {
        case CompilePick::Type: return city_.schema().types[static_cast<size_t>(selected_.type)].name;
        case CompilePick::Band:
            return city_.schema().types[static_cast<size_t>(selected_.type)].name + "/" + fnName(selected_.fn);
        case CompilePick::SharedBand: return t.files[static_cast<size_t>(selected_.file)].path + "/" + fnName(selected_.fn);
        case CompilePick::Skeleton: return t.skeletons[static_cast<size_t>(selected_.skeleton)].path;
        case CompilePick::None: break;
    }
    return "";
}

void CompileView::pick(const StrataRayHit& hit) {
    CompilePick p;
    auto from = [&](const MeshBuilder& m) { p = decodeTag(tagForTriangle(m.tags(), hit.triangle)); };
    // The tags are rebuilt from the same state the meshes were, so a
    // freshly built mesh decodes the hit's triangle.
    if (hit.mesh_id) {
        for (size_t d = 0; d < districtIds_.size(); ++d)
            if (districtIds_[d] == hit.mesh_id) {
                MeshBuilder m;
                city_.buildDistrict(d, m);
                from(m);
            }
        if (hit.mesh_id == libraryId_) {
            MeshBuilder m;
            city_.buildLibrary(m);
            from(m);
        }
        if (hit.mesh_id == marksId_) {
            MeshBuilder m;
            city_.buildMarks(selected_, m);
            from(m);
        }
    }
    select(p);
}

bool CompileView::key(int32_t sc) {
    Replay& r = replay();
    switch (sc) {
        case kScanSpace: r.togglePlaying(); haveLast_ = false; return true;
        case kScanLeftBracket: r.stepStage(-1); return true;
        case kScanRightBracket: r.stepStage(1); return true;
        case kScanComma: r.stepEvent(-1); return true;
        case kScanPeriod: r.stepEvent(1); return true;
        case kScanEnd: r.toEnd(); return true;
        case kScanL: r.setLookupFiltered(!r.lookupFiltered()); marksDirty_ = true; return true;
        case kScanC:
            // Compare mode (RFC 0034 G4) is a CompilePair's (view/compare_view.h); alone, there is no B.
            note_ = "compare mode (C) needs a second trace: asn1-viz --compare A.json B.json";
            noteUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            return true;
        case kScanF: return selected_.kind != CompilePick::None;
        default: return false;
    }
}

bool CompileView::focus(Vec3& target) const {
    switch (selected_.kind) {
        case CompilePick::Type:
        case CompilePick::Band: target = city_.city().roof(selected_.type); break;
        case CompilePick::Skeleton: target = city_.skeletonTop(selected_.skeleton); break;
        case CompilePick::SharedBand: target = city_.sharedTop(selected_.file); break;
        case CompilePick::None: return false;
    }
    target.y = 0;
    return true;
}

void CompileView::sync(strata::Engine& engine) {
    const auto now = std::chrono::steady_clock::now();
    if (clock_ && replay().playing()) {
        const float dt = haveLast_ ? std::chrono::duration<float>(now - last_).count() : 0.0f;
        replay().advance(std::min(dt, 0.1f));  // a stalled tab does not jump ahead
        last_ = now;
        haveLast_ = true;
    } else {
        haveLast_ = false;
    }
    const bool geometry = city_.geometryKey() != builtKey_;
    // Pulses fade, so the marks follow the playhead every frame of the Fix stage.
    if (geometry || marksDirty_ || replay().stage() == Stage::Fix) rebuild(engine, geometry);
}

// ------------------------------------------------------------------ the scrubber

// Each non-empty stage gets at least an eighth of the bar, so the Fix stage's
// thousands of events do not squeeze Parse and Save out of reach.
float CompileView::barFraction(float position) const {
    const Replay& r = replay();
    float w[kStages], total = 0;
    for (int s = 0; s < kStages; ++s) {
        const int n = r.stageSteps(static_cast<Stage>(s));
        w[s] = n > 0 ? std::max(0.125f, static_cast<float>(n) / static_cast<float>(std::max(1, r.steps()))) : 0;
        total += w[s];
    }
    if (total <= 0) return 1;
    float at = 0;
    for (int s = 0; s < kStages; ++s) {
        const float a = static_cast<float>(r.stageStart(static_cast<Stage>(s)));
        const float n = static_cast<float>(r.stageSteps(static_cast<Stage>(s)));
        if (n > 0 && position < a + n) return (at + w[s] * std::max(0.0f, position - a) / n) / total;
        at += w[s];
    }
    return 1;
}

float CompileView::positionAt(float f) const {
    const Replay& r = replay();
    float w[kStages], total = 0;
    for (int s = 0; s < kStages; ++s) {
        const int n = r.stageSteps(static_cast<Stage>(s));
        w[s] = n > 0 ? std::max(0.125f, static_cast<float>(n) / static_cast<float>(std::max(1, r.steps()))) : 0;
        total += w[s];
    }
    float want = std::clamp(f, 0.0f, 1.0f) * total, at = 0;
    for (int s = 0; s < kStages; ++s) {
        if (w[s] > 0 && want <= at + w[s])
            return static_cast<float>(r.stageStart(static_cast<Stage>(s))) +
                   (want - at) / w[s] * static_cast<float>(r.stageSteps(static_cast<Stage>(s)));
        at += w[s];
    }
    return static_cast<float>(r.steps());
}

bool CompileView::press(float px, float py, bool dragging) {
    if (barX1_ <= barX0_) return false;
    if (!dragging && (px < barX0_ || px > barX1_ || py < barY0_ || py > barY1_)) return false;
    replay().seek(positionAt((px - barX0_) / (barX1_ - barX0_)));
    return true;
}

// ------------------------------------------------------------------ drawing

void CompileView::draw(Overlay& o, const CameraMatrices& cam, int w, int h) {
    const Schema& s = city_.schema();
    const Replay& r = replay();
    const CompileTrace& tr = city_.trace();
    const float ui = o.ui;
    std::vector<Rect> taken;
    if (panelOnCanvas()) taken.push_back({0, 0, static_cast<float>(w) * 0.62f, (16 + 8 * 20.0f) * ui});

    // The scrubber, along the bottom: a row of stage-coloured cells, '=' done
    // and '-' to come, with the playhead as '|', and the stage names above.
    const float cellPx = 16 * ui;
    const float margin = 16 * ui;
    const int cells = std::max(8, static_cast<int>((static_cast<float>(w) - 2 * margin) / cellPx));
    barX0_ = margin;
    barX1_ = margin + static_cast<float>(cells) * cellPx;
    const float barY = static_cast<float>(h) - 30 * ui;
    barY0_ = barY - 18 * ui;  // a generous target for a finger, names included
    barY1_ = static_cast<float>(h);
    taken.push_back({0, barY0_, static_cast<float>(w), static_cast<float>(h)});
    const float head = barFraction(r.position());
    const int headCell = std::min(cells - 1, static_cast<int>(head * static_cast<float>(cells)));
    std::string run;
    int runStage = -1;
    bool runDone = false;
    float runX = barX0_;
    auto flush = [&]() {
        if (run.empty()) return;
        float c[4];
        std::copy(kStageInk[runStage], kStageInk[runStage] + 4, c);
        if (!runDone) c[0] *= 0.45f, c[1] *= 0.45f, c[2] *= 0.45f;
        o.text(runX, barY, 16, run, c, false);
        run.clear();
    };
    for (int i = 0; i < cells; ++i) {
        const float mid = (static_cast<float>(i) + 0.5f) / static_cast<float>(cells);
        const float p = positionAt(mid);
        int st = 0;
        for (int k = 0; k < kStages; ++k)
            if (r.stageSteps(static_cast<Stage>(k)) > 0 && p >= static_cast<float>(r.stageStart(static_cast<Stage>(k))))
                st = k;
        const bool done = mid <= head;
        if (i == headCell) {
            flush();
            o.text(barX0_ + static_cast<float>(i) * cellPx, barY, 16, "|", kGold, false);
            runX = barX0_ + static_cast<float>(i + 1) * cellPx;
            continue;
        }
        if (st != runStage || done != runDone) {
            flush();
            runStage = st;
            runDone = done;
            runX = barX0_ + static_cast<float>(i) * cellPx;
        }
        run += done ? '=' : '-';
    }
    flush();
    // Stage names over their parts of the bar; on a narrow bar a name that
    // would run into the next is cut to its initial.
    float nameEnd = -1e9f;
    for (int k = 0; k < kStages; ++k) {
        const int n = r.stageSteps(static_cast<Stage>(k));
        if (n <= 0) continue;
        const float x = barX0_ + barFraction(static_cast<float>(r.stageStart(static_cast<Stage>(k)))) * (barX1_ - barX0_);
        float next = barX1_;
        for (int j = k + 1; j < kStages; ++j)
            if (r.stageSteps(static_cast<Stage>(j)) > 0) {
                next = barX0_ + barFraction(static_cast<float>(r.stageStart(static_cast<Stage>(j)))) * (barX1_ - barX0_);
                break;
            }
        const bool failed = (k == 1 && !statusOk(tr.fixStatus)) || (k == 2 && !statusOk(tr.compileStatus));
        std::string name = std::string(stageName(static_cast<Stage>(k))) + (failed ? " (failed)" : "");
        if (x + 8 * ui * static_cast<float>(name.size()) > next - 4 * ui) name = name.substr(0, 1);
        if (x < nameEnd) continue;
        o.text(x, barY - 14 * ui, 8, name, failed ? kFail : kStageInk[k]);
        nameEnd = x + 8 * ui * static_cast<float>(name.size() + 1);
    }

    // District and library names.
    for (size_t d = 0; d < city_.city().districts().size(); ++d) {
        const District& di = city_.city().districts()[d];
        float x, y;
        if (!toScreen(cam, {(di.x0 + di.x1) / 2, 0, di.z1 + 1.5f}, w, h, x, y)) continue;
        const std::string t = s.modules[static_cast<size_t>(di.module)].name;
        const float tw = 16.0f * ui * static_cast<float>(t.size());
        const Rect rc{x - tw / 2, y, x + tw / 2, y + 16 * ui};
        if (overlaps(taken, rc)) continue;
        o.text(rc.x0, rc.y0, 16, t, kInk);
        taken.push_back(rc);
    }
    auto shelfLabel = [&](const CompileCity::Shelf& sh, const std::string& t) {
        float x, y;
        if (!toScreen(cam, {(sh.x0 + sh.x1) / 2, 0, sh.z1 + 1.2f}, w, h, x, y)) return;
        const float tw = 8.0f * ui * static_cast<float>(t.size());
        const Rect rc{x - tw / 2, y, x + tw / 2, y + 8 * ui};
        if (overlaps(taken, rc)) return;
        o.text(rc.x0, rc.y0, 8, t, kDim);
        taken.push_back(rc);
    };
    if (!city_.sharedFiles().empty())
        shelfLabel(city_.sharedShelf(), "generated, no one type (" + std::to_string(city_.sharedFiles().size()) + ")");
    if (!tr.skeletons.empty())
        shelfLabel(city_.skeletonShelf(), "skeletons, copied (" + commas(r.skeletonsDone()) + " of " +
                                              commas(static_cast<long long>(tr.skeletons.size())) + ")");

    // Building names: the selection, the current event's type, then the biggest.
    std::vector<int> order;
    if (selected_.type >= 0) order.push_back(selected_.type);
    const int ev = r.stage() == Stage::Fix ? r.currentEvent() : -1;
    const int evType = ev >= 0 ? tr.events[static_cast<size_t>(ev)].type : -1;
    if (evType >= 0) order.push_back(evType);
    std::vector<int> big;
    for (const SchemaType& t : s.types)
        if (!t.flat()) big.push_back(t.id);
    std::stable_sort(big.begin(), big.end(), [&](int a, int b) { return city_.height(a) > city_.height(b); });
    order.insert(order.end(), big.begin(), big.end());
    std::vector<bool> done(s.types.size(), false);
    int placed = 0;
    for (int t : order) {
        if (done[static_cast<size_t>(t)] || placed > 40) continue;
        done[static_cast<size_t>(t)] = true;
        if (r.rise(t) <= 0) continue;  // not parsed yet
        const Vec3 p = city_.city().roof(t);
        float x, y;
        if (!toScreen(cam, {p.x, city_.height(t) + 0.6f, p.z}, w, h, x, y)) continue;
        const bool sel = t == selected_.type, now = t == evType;
        const float size = sel || now ? 16.0f : 8.0f;
        const std::string& name = s.types[static_cast<size_t>(t)].name;
        const float tw = size * ui * static_cast<float>(name.size());
        const Rect rc{x - tw / 2, y - size * ui - 2, x + tw / 2, y - 2};
        if (!sel && (overlaps(taken, rc) || rc.x0 < 0 || rc.x1 > static_cast<float>(w))) continue;
        float c[4];
        if (now && !sel) rgba(familyColor(passFamily(tr.events[static_cast<size_t>(ev)].fn)), c);
        else std::copy(sel ? kGold : kInk, (sel ? kGold : kInk) + 4, c);
        o.text(rc.x0, rc.y0, size, name, c);
        taken.push_back(rc);
        ++placed;
    }

    // The panel.
    if (panelOnCanvas()) {
        float py = 16 * ui;
        const size_t fit = static_cast<size_t>(std::max(8.0f, (static_cast<float>(w) * 0.62f - 32 * ui) / (8 * ui)));
        for (const PanelLine& l : panel(fit)) {
            o.text(16 * ui, py, l.size, l.text, l.color);
            py += (l.size + 4) * ui;
        }
    }

    if (compact_) return;
    // The legend for this stage, above the scrubber.
    std::vector<std::pair<std::string, Rgb>> legend;
    if (r.stage() == Stage::Fix || r.stage() == Stage::Parse) {
        for (int f = 0; f < kPassFamilies; ++f) {
            const PassFamily pf = static_cast<PassFamily>(f);
            if (pf == PassFamily::Lookup && r.lookupFiltered()) continue;
            legend.push_back({std::string("pass: ") + passFamilyName(pf), familyColor(pf)});
        }
        legend.push_back({"changed by the fixer (roof mark)", {kMark[0], kMark[1], kMark[2]}});
    } else {
        // The functions that emitted the most lines so far.
        std::vector<long long> lines(city_.functions().size() + 1, 0);
        for (int f = 0; f < r.filesDone(); ++f) {
            const CompileFile& file = tr.files[static_cast<size_t>(f)];
            for (const auto& kv : file.byFn) {
                const int i = city_.functionIndex(kv.first);
                if (i >= 0) lines[static_cast<size_t>(i)] += kv.second;
            }
            lines.back() += file.unannotated;
        }
        std::vector<int> idx;
        for (size_t i = 0; i < lines.size(); ++i)
            if (lines[i] > 0) idx.push_back(static_cast<int>(i));
        std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return lines[static_cast<size_t>(a)] > lines[static_cast<size_t>(b)]; });
        const int fns = static_cast<int>(city_.functions().size());
        for (size_t k = 0; k < idx.size() && k < 10; ++k) {
            const int i = idx[k];
            legend.push_back({(i < fns ? city_.functions()[static_cast<size_t>(i)] : std::string("unannotated (includes, headers)")) +
                                  "  " + commas(lines[static_cast<size_t>(i)]),
                              functionColor(i, fns)});
        }
        if (!tr.skeletons.empty()) legend.push_back({"skeleton (copied, not generated)", {0.74f, 0.6f, 0.42f}});
    }
    float ly = barY0_ - (12.0f * static_cast<float>(legend.size() + 1) + 6) * ui;
    for (const auto& k : legend) {
        float c[4];
        rgba(k.second, c);
        o.text(16 * ui, ly, 8, "##", c, false);
        o.text(36 * ui, ly, 8, k.first, kDim);
        ly += 12 * ui;
    }
    o.text(16 * ui, ly, 8,
           "space play  [ ] stage  , . step  L lookups  drag the bar to scrub  click pick  F focus  H home", kDim);
}

// A picked band's top origins (RFC 0034 section 3): the compiler source
// lines in its function that wrote the most of its lines.
template <class Line>
static void originLines(const CompileTrace& tr, const std::vector<int>& files, const std::string& fn, Line& line) {
    if (fn.empty() || tr.sources.empty()) return;
    const auto top = tr.originsOf(files, fn, 5);
    if (top.empty()) return;
    line("top origins in asn1c (source:line):", kDim, 8);
    for (const auto& [src, n] : top) line("  " + src + "  " + commas(n) + (n == 1 ? " line" : " lines"), kDim, 8);
}

std::vector<PanelLine> CompileView::panel(size_t fit) const {
    std::vector<PanelLine> out;
    auto line = [&](std::string t, const float* c, float size = 16) {
        PanelLine l;
        for (char& ch : t)
            if (ch == '\t') ch = ' ';  // the schema prints use tabs
        // Small lines are cut to fit; big ones (a name, the stage) are short.
        l.text = size == 8 && fit > 3 && t.size() > fit ? t.substr(0, fit - 3) + "..." : t;
        std::copy(c, c + 4, l.color);
        l.size = size;
        out.push_back(std::move(l));
    };
    const Schema& s = city_.schema();
    const Replay& r = replay();
    const CompileTrace& tr = city_.trace();
    const Stage st = r.stage();
    const int si = static_cast<int>(st);
    auto typeName = [&](int t) { return t >= 0 ? s.types[static_cast<size_t>(t)].name : std::string("unattributed"); };

    // The stage and where the playhead is in it.
    std::string head = std::string(stageName(st)) + "  " + std::to_string(si + 1) + "/4   ";
    const int done = r.doneIn(st), n = r.stageSteps(st);
    const char* unit = st == Stage::Parse ? " assignments" : st == Stage::Fix ? " events" : st == Stage::Compile ? " files" : " skeletons";
    head += commas(done) + " / " + commas(n) + unit + (r.playing() ? "   playing" : "   paused");
    line(head, kStageInk[si]);

    // What the playhead is on.
    if (st == Stage::Parse) {
        const int k = done - 1;
        if (k >= 0) {
            const CompileParseStep& p = tr.parse[static_cast<size_t>(k)];
            line("parsed " + typeName(p.type) + (p.type >= 0 ? " ::= " + s.types[static_cast<size_t>(p.type)].kind : "") +
                     "   line " + std::to_string(p.line),
                 kDim, 8);
        } else {
            line("asn1p_parse_file: assignments appear in source order", kDim, 8);
        }
    } else if (st == Stage::Fix) {
        const int ev = r.currentEvent();
        if (ev >= 0) {
            const CompileEvent& e = tr.events[static_cast<size_t>(ev)];
            float c[4];
            rgba(familyColor(passFamily(e.fn)), c);
            line(e.fn + " (" + typeName(e.type) + ")" + (e.src.empty() ? "" : " - " + e.src), c, 8);
            line(std::string("pass family: ") + passFamilyName(passFamily(e.fn)) + "   event " + commas(e.seq) +
                     (r.lookupFiltered() ? "   lookups hidden (L)" : "   L hides lookups"),
                 kDim, 8);
        } else {
            line("asn1f_process: the fixer's passes, event by event" +
                     std::string(r.lookupFiltered() ? "   lookups hidden (L)" : ""),
                 kDim, 8);
        }
        if (!statusOk(tr.fixStatus)) line("the fixer failed: " + tr.fixStatus, kFail, 8);
    } else if (st == Stage::Compile) {
        const int f = done - 1;
        if (f >= 0) {
            const CompileFile& file = tr.files[static_cast<size_t>(f)];
            line("wrote " + file.path + " (" + typeName(file.type) + "): " + commas(file.lines) + " lines", kDim, 8);
        }
        if (!statusOk(tr.compileStatus)) line("the compiler failed: " + tr.compileStatus, kFail, 8);
    } else {
        long long bytes = 0, lines = 0;
        for (int i = 0; i < r.skeletonsDone(); ++i) bytes += tr.skeletons[static_cast<size_t>(i)].bytes;
        for (const CompileFile& f : tr.files) lines += f.lines;
        line("generated " + commas(static_cast<long long>(tr.files.size())) + " files, " + commas(lines) +
                 " lines; copied " + commas(r.skeletonsDone()) + " skeletons, " + commas(bytes) + " bytes",
             kDim, 8);
    }

    const std::string note = std::chrono::steady_clock::now() < noteUntil_ ? note_ : "";
    if (!note.empty()) line(note, kGold, 8);

    // The selection.
    auto fnName = [&](int fn) {
        return fn >= 0 && fn < static_cast<int>(city_.functions().size()) ? city_.functions()[static_cast<size_t>(fn)]
                                                                     : std::string("unannotated lines");
    };
    if (selected_.kind == CompilePick::Type || selected_.kind == CompilePick::Band) {
        const int t = selected_.type;
        const SchemaType& ty = s.types[static_cast<size_t>(t)];
        const SchemaModule& m = s.modules[static_cast<size_t>(ty.module)];
        line(ty.name + " ::= " + ty.kind, kGold);
        line(m.name + "   " + m.file + ":" + std::to_string(ty.line), kDim, 8);
        // The fixer's visits so far, by family.
        int fam[kPassFamilies] = {0};
        int visits = 0;
        const int upto = r.eventsDone();
        for (int i = 0; i < upto; ++i)
            if (tr.events[static_cast<size_t>(i)].type == t) {
                ++fam[static_cast<int>(passFamily(tr.events[static_cast<size_t>(i)].fn))];
                ++visits;
            }
        if (r.stage() != Stage::Parse) {
            std::string v = "fixer: " + commas(visits) + " events";
            std::string sep = " (";
            for (int f = 0; f < kPassFamilies; ++f)
                if (fam[f]) {
                    v += sep + std::to_string(fam[f]) + " " + passFamilyName(static_cast<PassFamily>(f));
                    sep = ", ";
                }
            if (sep == ", ") v += ")";
            line(v, kDim, 8);
        }
        if (r.diffShown(t)) {
            int add = 0, del = 0;
            diffCounts(tr, t, add, del);
            line("changed by the fixer: +" + std::to_string(add) + " -" + std::to_string(del) + " printed lines", kMark, 8);
            const auto it = tr.diffs.find(t);
            const std::string before = firstChange(it->second, '-'), after = firstChange(it->second, '+');
            if (!before.empty()) line("  - " + before, kDim, 8);
            if (!after.empty()) line("  + " + after, kDim, 8);
        } else if (r.stage() != Stage::Parse && !fixerChanged(tr, t) && r.stage() != Stage::Fix) {
            line("unchanged by the fixer", kDim, 8);
        }
        const std::vector<int> lines = city_.emitted(t);
        int total = 0;
        for (int l : lines) total += l;
        if (selected_.kind == CompilePick::Band) {
            const int fn = selected_.fn;
            const int nfn = fn >= 0 && fn < static_cast<int>(lines.size()) ? lines[static_cast<size_t>(fn)] : 0;
            float c[4];
            rgba(functionColor(fn, static_cast<int>(city_.functions().size())), c);
            line(fnName(fn) + ": " + commas(nfn) + " of " + commas(total) + " lines", c, 8);
            std::vector<int> files;
            for (int i : tr.filesOf(t))
                if (i < r.filesDone()) files.push_back(i);
            originLines(tr, files, fn >= 0 ? fnName(fn) : std::string(), line);
        } else if (total > 0) {
            std::vector<int> idx;
            for (size_t i = 0; i < lines.size(); ++i)
                if (lines[i] > 0) idx.push_back(static_cast<int>(i));
            std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return lines[static_cast<size_t>(a)] > lines[static_cast<size_t>(b)]; });
            std::string c = "C: " + commas(total) + " lines; ";
            for (size_t k = 0; k < idx.size() && k < 3; ++k)
                c += (k ? ", " : "") + fnName(idx[k]) + " " + commas(lines[static_cast<size_t>(idx[k])]);
            line(c, kDim, 8);
        }
    } else if (selected_.kind == CompilePick::SharedBand) {
        const CompileFile& f = tr.files[static_cast<size_t>(selected_.file)];
        line(f.path, kGold);
        const auto it = selected_.fn < static_cast<int>(city_.functions().size())
                            ? f.byFn.find(city_.functions()[static_cast<size_t>(selected_.fn)])
                            : f.byFn.end();
        const int nfn = it != f.byFn.end() ? it->second : f.unannotated;
        line("generated, no one type: " + commas(f.lines) + " lines; " + fnName(selected_.fn) + " " + commas(nfn), kDim, 8);
        if (selected_.fn < static_cast<int>(city_.functions().size()))
            originLines(tr, {selected_.file}, fnName(selected_.fn), line);
    } else if (selected_.kind == CompilePick::Skeleton) {
        const CompileCopied& k = tr.skeletons[static_cast<size_t>(selected_.skeleton)];
        line(k.path, kGold);
        line("skeleton: " + commas(k.bytes) + " bytes, copied from asn1c's skeletons/, not generated", kDim, 8);
    } else if (st == Stage::Parse || done == 0) {
        line("click a building: its parse, fixer passes, diff and generated C", kDim, 8);
    }
    return out;
}

std::string CompileView::statusJson() const {
    const Replay& r = replay();
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "{\"stage\":\"%s\",\"pos\":%.3f,\"steps\":%d,\"fraction\":%.5f,\"bar\":%.5f,\"playing\":%s,\"lookups\":%s}",
                  stageName(r.stage()), static_cast<double>(r.position()), r.steps(), static_cast<double>(r.fraction()),
                  static_cast<double>(barFraction(r.position())), r.playing() ? "true" : "false",
                  r.lookupFiltered() ? "false" : "true");
    return buf;
}

}  // namespace asn1viz
