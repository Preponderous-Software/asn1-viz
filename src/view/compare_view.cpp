#include "view/compare_view.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace asn1viz {

namespace {

// SDL3 scancodes.
constexpr int32_t kScanF = 9, kScanC = 6;

const float kInk[4] = {0.92f, 0.94f, 0.97f, 1};
const float kDim[4] = {0.62f, 0.66f, 0.74f, 1};
const float kGold[4] = {1, 0.82f, 0.25f, 1};

std::string commas(long long n) {
    std::string s = std::to_string(n < 0 ? -n : n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return n < 0 ? "-" + s : s;
}

std::string signedCommas(long long n) { return n > 0 ? "+" + commas(n) : n == 0 ? "0" : commas(n); }

// "1,093 -> 1,047 (-46)"
std::string fromTo(long long a, long long b) { return commas(a) + " -> " + commas(b) + " (" + signedCommas(b - a) + ")"; }

void rgba(const Rgb& c, float out[4]) {
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
    out[3] = 1;
}

const float* deltaInk(long long d, float buf[4]) {
    if (d == 0) return kDim;
    rgba(d > 0 ? growthColor() : shrinkColor(), buf);
    return buf;
}

struct Rect {
    float x0, y0, x1, y1;
};

bool overlaps(const std::vector<Rect>& taken, const Rect& r) {
    for (const Rect& t : taken)
        if (r.x0 < t.x1 && r.x1 > t.x0 && r.y0 < t.y1 && r.y1 > t.y0) return true;
    return false;
}

}  // namespace

// ------------------------------------------------------------------ CompareView

CompareView::CompareView(const Schema& schema, CompileCompare compare, CityStyle style)
    : compare_(std::move(compare)), city_(schema, compare_, style) {}

void CompareView::upload(strata::Engine& engine) {
    districtIds_.clear();
    for (size_t d = 0; d < city_.city().districts().size(); ++d) {
        MeshBuilder m;
        city_.buildDistrict(d, m);
        districtIds_.push_back(uploadMesh(engine, m));
    }
    marksId_ = 0;
    rebuildMarks(engine);
}

void CompareView::rebuildMarks(strata::Engine& engine) {
    if (marksId_) engine.removeMesh(marksId_);
    MeshBuilder marks;
    city_.buildMarks(selected_, marks);
    marksId_ = uploadMesh(engine, marks, true);
    marksDirty_ = false;
}

void CompareView::sync(strata::Engine& engine) {
    if (marksDirty_) rebuildMarks(engine);
}

OrbitCamera CompareView::home(float aspect) const { return city_.homeCamera(aspect); }

void CompareView::select(const CompilePick& p) {
    selected_ = p;
    marksDirty_ = true;
}

bool CompareView::selectName(const std::string& name) {
    const size_t slash = name.find('/');
    const int t = city_.schema().typeByName(name.substr(0, slash));
    if (t < 0) return false;
    CompilePick p;
    p.kind = CompilePick::Type;
    p.type = t;
    if (slash != std::string::npos) {
        const std::string fn = name.substr(slash + 1);
        p.fn = fn == "unannotated" ? static_cast<int>(compare_.functions().size()) : compare_.functionIndex(fn);
        if (p.fn < 0) return false;
        p.kind = CompilePick::Band;
    }
    select(p);
    return true;
}

std::string CompareView::selectionName() const {
    if (selected_.type < 0 || (selected_.kind != CompilePick::Type && selected_.kind != CompilePick::Band)) return "";
    const std::string& t = city_.schema().types[static_cast<size_t>(selected_.type)].name;
    return selected_.kind == CompilePick::Band ? t + "/" + compare_.functionName(selected_.fn) : t;
}

void CompareView::pick(const StrataRayHit& hit) {
    CompilePick p;
    if (hit.mesh_id) {
        for (size_t d = 0; d < districtIds_.size(); ++d)
            if (districtIds_[d] == hit.mesh_id) {
                MeshBuilder m;
                city_.buildDistrict(d, m);
                p = decodeTag(tagForTriangle(m.tags(), hit.triangle));
            }
        if (hit.mesh_id == marksId_) {
            MeshBuilder m;
            city_.buildMarks(selected_, m);
            p = decodeTag(tagForTriangle(m.tags(), hit.triangle));
        }
    }
    if (p.kind != CompilePick::Type && p.kind != CompilePick::Band) p = CompilePick{};
    select(p);
}

bool CompareView::key(int32_t sc) { return sc == kScanF && selected_.kind != CompilePick::None; }

bool CompareView::focus(Vec3& target) const {
    if (selected_.type < 0) return false;
    target = city_.city().roof(selected_.type);
    target.y = 0;
    return true;
}

void CompareView::draw(Overlay& o, const CameraMatrices& cam, int w, int h) {
    const Schema& s = city_.schema();
    const float ui = o.ui;
    std::vector<Rect> taken;
    if (panelOnCanvas()) taken.push_back({0, 0, static_cast<float>(w) * 0.62f, (16 + 9 * 20.0f) * ui});
    const float legendTop = static_cast<float>(h) - (compact_ ? 16.0f : 12.0f * 8 + 24) * ui;
    taken.push_back({0, legendTop, static_cast<float>(w) * 0.5f, static_cast<float>(h)});

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

    // Names: the selection, then the changed types with the biggest change, then the tallest.
    std::vector<int> order;
    if (selected_.type >= 0) order.push_back(selected_.type);
    std::vector<int> rest;
    for (const SchemaType& t : s.types) rest.push_back(t.id);
    std::stable_sort(rest.begin(), rest.end(), [&](int a, int b) {
        const CompareEntry &ea = compare_.type(a), &eb = compare_.type(b);
        if (ea.changed() != eb.changed()) return ea.changed();
        if (ea.changed()) return std::abs(ea.delta()) > std::abs(eb.delta());
        return city_.height(a) > city_.height(b);
    });
    order.insert(order.end(), rest.begin(), rest.end());
    std::vector<bool> done(s.types.size(), false);
    int placed = 0;
    for (int t : order) {
        if (done[static_cast<size_t>(t)] || placed > 40) continue;
        done[static_cast<size_t>(t)] = true;
        if (s.types[static_cast<size_t>(t)].flat() && t != selected_.type) continue;
        const Vec3 p = city_.city().roof(t);
        float x, y;
        if (!toScreen(cam, {p.x, city_.height(t) + 0.6f, p.z}, w, h, x, y)) continue;
        const CompareEntry& e = compare_.type(t);
        const bool sel = t == selected_.type;
        std::string name = s.types[static_cast<size_t>(t)].name;
        if (e.changed()) name += " " + signedCommas(e.delta());
        const float size = sel ? 16.0f : 8.0f;
        const float tw = size * ui * static_cast<float>(name.size());
        const Rect rc{x - tw / 2, y - size * ui - 2, x + tw / 2, y - 2};
        if (!sel && (overlaps(taken, rc) || rc.x0 < 0 || rc.x1 > static_cast<float>(w))) continue;
        float c[4], buf[4];
        if (sel) std::copy(kGold, kGold + 4, c);
        else if (e.changed()) std::copy(deltaInk(e.delta(), buf), deltaInk(e.delta(), buf) + 4, c);
        else std::copy(kDim, kDim + 4, c);
        o.text(rc.x0, rc.y0, size, name, c);
        taken.push_back(rc);
        ++placed;
    }

    if (panelOnCanvas()) {
        float py = 16 * ui;
        const size_t fit = static_cast<size_t>(std::max(8.0f, (static_cast<float>(w) * 0.62f - 32 * ui) / (8 * ui)));
        for (const PanelLine& l : panel(fit)) {
            o.text(16 * ui, py, l.size, l.text, l.color);
            py += (l.size + 4) * ui;
        }
    }

    if (compact_) return;
    const std::pair<const char*, Rgb> legend[] = {
        {"B wrote more lines (growth)", growthColor()},
        {"B wrote fewer lines (shrinkage)", shrinkColor()},
        {"lines both wrote, per function (changed type)", sharedColor(0, 8, true)},
        {"type unchanged", sharedColor(0, 8, false)},
        {"roof: a changed type's net change (pale: lines only moved)", netColor(0)},
        {"the fixer differs (diff or events)", fixerDiffColor()},
    };
    float ly = legendTop;
    for (const auto& k : legend) {
        float c[4];
        rgba(k.second, c);
        o.text(16 * ui, ly, 8, "##", c, false);
        o.text(36 * ui, ly, 8, k.first, kDim);
        ly += 12 * ui;
    }
    o.text(16 * ui, ly, 8, "C back to the replay  click a floor  F focus  H home", kDim);
}

std::vector<PanelLine> CompareView::panel(size_t fit) const {
    std::vector<PanelLine> out;
    auto line = [&](std::string t, const float* c, float size = 16) {
        PanelLine l;
        l.text = size == 8 && fit > 3 && t.size() > fit ? t.substr(0, fit - 3) + "..." : t;
        std::copy(c, c + 4, l.color);
        l.size = size;
        out.push_back(std::move(l));
    };
    const Schema& s = city_.schema();
    const CompileTrace &a = compare_.a(), &b = compare_.b();
    float buf[4];

    line("Compare  A -> B", kInk);
    line("A: " + CompileCompare::flagsLabel(a) + (a.asn1c.empty() ? "" : " @ " + a.asn1c.substr(0, 8)) +
             "   B: " + CompileCompare::flagsLabel(b) + (b.asn1c.empty() ? "" : " @ " + b.asn1c.substr(0, 8)),
         kDim, 8);
    int withCode = 0;
    for (int t = 0; t < compare_.typeCount(); ++t) {
        const CompareEntry& e = compare_.type(t);
        withCode += e.linesA > 0 || e.linesB > 0 ? 1 : 0;
    }
    line("types changed: " + commas(compare_.typesChanged()) + " of " + commas(withCode) + " with code   lines " +
             fromTo(compare_.totalA(), compare_.totalB()),
         deltaInk(compare_.totalB() - compare_.totalA(), buf), 8);
    const int fx = compare_.typesFixerDiffers();
    line(fx ? "the fixer differs on " + commas(fx) + " types; events " +
                  fromTo(static_cast<long long>(a.events.size()), static_cast<long long>(b.events.size()))
            : "the fixer: same diff and events on every type (events " +
                  fromTo(static_cast<long long>(a.events.size()), static_cast<long long>(b.events.size())) + ")",
         kDim, 8);
    line("files " + fromTo(static_cast<long long>(a.files.size()), static_cast<long long>(b.files.size())) +
             "   skeletons copied " +
             fromTo(static_cast<long long>(a.skeletons.size()), static_cast<long long>(b.skeletons.size())),
         kDim, 8);
    if (compare_.shared().changed())
        line("files no one type owns: " + fromTo(compare_.shared().linesA, compare_.shared().linesB) + " lines", kDim, 8);

    if (selected_.type < 0) {
        line("click a floor: its function, A and B lines and the delta", kDim, 8);
        return out;
    }
    const int t = selected_.type;
    const SchemaType& ty = s.types[static_cast<size_t>(t)];
    const SchemaModule& m = s.modules[static_cast<size_t>(ty.module)];
    const CompareEntry& e = compare_.type(t);
    line(ty.name + " ::= " + ty.kind, kGold);
    line(m.name + "   " + m.file + ":" + std::to_string(ty.line), kDim, 8);
    line("C: " + fromTo(e.linesA, e.linesB) + " lines in " + fromTo(e.filesA, e.filesB) + " files",
         deltaInk(e.delta(), buf), 8);
    if (selected_.kind == CompilePick::Band) {
        const size_t fn = static_cast<size_t>(selected_.fn);
        const int la = fn < e.a.size() ? e.a[fn] : 0, lb = fn < e.b.size() ? e.b[fn] : 0;
        line(compare_.functionName(selected_.fn) + ": A " + commas(la) + ", B " + commas(lb) + ", delta " +
                 signedCommas(lb - la),
             deltaInk(lb - la, buf), 8);
    } else if (e.changed()) {
        // The functions that changed most.
        std::vector<int> idx;
        for (size_t i = 0; i < e.a.size(); ++i)
            if (e.a[i] != e.b[i]) idx.push_back(static_cast<int>(i));
        std::stable_sort(idx.begin(), idx.end(), [&](int x, int y) {
            return std::abs(e.b[static_cast<size_t>(x)] - e.a[static_cast<size_t>(x)]) >
                   std::abs(e.b[static_cast<size_t>(y)] - e.a[static_cast<size_t>(y)]);
        });
        std::string c;
        for (size_t k = 0; k < idx.size() && k < 3; ++k)
            c += (k ? ", " : "") + compare_.functionName(idx[k]) + " " +
                 signedCommas(e.b[static_cast<size_t>(idx[k])] - e.a[static_cast<size_t>(idx[k])]);
        line(c, kDim, 8);
    }
    line("fixer: events A " + commas(e.eventsA) + ", B " + commas(e.eventsB) +
             (e.diffDiffers ? "; its printed diff differs" : "; same printed diff"),
         e.fixerDiffers() ? kGold : kDim, 8);
    return out;
}

// ------------------------------------------------------------------ CompilePair

CompilePair::CompilePair(std::unique_ptr<CompileView> replay, std::unique_ptr<CompareView> compare, bool comparing)
    : replay_(std::move(replay)), compare_(std::move(compare)), comparing_(comparing) {}

void CompilePair::upload(strata::Engine& engine) {
    swapped_ = false;
    active().upload(engine);
}

void CompilePair::setComparing(bool on) {
    if (on == comparing_) return;
    // The selection carries over: a type, or a floor by its function's name.
    std::string fnName;
    CompilePick p = comparing_ ? compare_->selected() : replay_->selected();
    if (p.kind == CompilePick::Band) {
        fnName = comparing_ ? compare_->compare().functionName(p.fn)
                            : (p.fn >= 0 && p.fn < static_cast<int>(replay_->scene().functions().size())
                                   ? replay_->scene().functions()[static_cast<size_t>(p.fn)]
                                   : std::string("unannotated"));
    }
    comparing_ = on;
    swapped_ = true;
    if (p.kind != CompilePick::Type && p.kind != CompilePick::Band) {
        if (on) compare_->select(CompilePick{});
        else replay_->select(CompilePick{});
        return;
    }
    if (on) {
        if (p.kind == CompilePick::Band) {
            p.fn = fnName == "unannotated" ? static_cast<int>(compare_->compare().functions().size())
                                           : compare_->compare().functionIndex(fnName);
            if (p.fn < 0) p.kind = CompilePick::Type;
        }
        compare_->select(p);
    } else {
        if (p.kind == CompilePick::Band) {
            p.fn = fnName == "unannotated" ? static_cast<int>(replay_->scene().functions().size())
                                           : replay_->scene().functionIndex(fnName);
            if (p.fn < 0) p.kind = CompilePick::Type;
        }
        replay_->select(p);
    }
}

bool CompilePair::key(int32_t sc) {
    if (sc == kScanC) {
        setComparing(!comparing_);
        return true;
    }
    return active().key(sc);
}

void CompilePair::sync(strata::Engine& engine) {
    if (swapped_) {
        engine.clearMeshes();
        upload(engine);
    }
    active().sync(engine);
}

void CompilePair::draw(Overlay& overlay, const CameraMatrices& camera, int width, int height) {
    active().setPanelOnCanvas(panelOnCanvas());
    active().draw(overlay, camera, width, height);
}

std::string CompilePair::selectionName() const {
    return comparing_ ? compare_->selectionName() : replay_->selectionName();
}

bool CompilePair::selectName(const std::string& name) {
    return comparing_ ? compare_->selectName(name) : replay_->selectName(name);
}

CompileView* replayOf(Scene& scene) {
    if (auto* v = dynamic_cast<CompileView*>(&scene)) return v;
    if (auto* p = dynamic_cast<CompilePair*>(&scene)) return p->comparing() ? nullptr : &p->replay();
    return nullptr;
}

const CompileView* replayOf(const Scene& scene) { return replayOf(const_cast<Scene&>(scene)); }

}  // namespace asn1viz
