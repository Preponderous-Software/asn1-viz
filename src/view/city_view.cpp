#include "view/city_view.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace asn1viz {

namespace {

// SDL3 scancodes.
constexpr int32_t kScan1 = 30, kScan2 = 31, kScan3 = 32, kScan4 = 33, kScanR = 21, kScanF = 9, kScanU = 24;

const float kInk[4] = {0.92f, 0.94f, 0.97f, 1};
const float kDim[4] = {0.62f, 0.66f, 0.74f, 1};
const float kOut[4] = {0.35f, 0.85f, 1, 0.95f};
const float kIn[4] = {1, 0.62f, 0.25f, 0.95f};
const float kImport[4] = {0.75f, 0.8f, 0.95f, 0.55f};
const float kLoop[4] = {1, 0.35f, 0.5f, 0.95f};

struct Rect {
    float x0, y0, x1, y1;
};

bool overlaps(const std::vector<Rect>& taken, const Rect& r) {
    for (const Rect& t : taken)
        if (r.x0 < t.x1 && r.x1 > t.x0 && r.y0 < t.y1 && r.y1 > t.y0) return true;
    return false;
}

void addArc(Overlay& o, Vec3 a, Vec3 b, float lift, const float c[4]) {
    const std::vector<Vec3> pts = arc(a, b, lift, 16);
    for (size_t i = 0; i + 1 < pts.size(); i += 2) o.line(pts[i], pts[i + 1], c);
}

std::string joinNames(const Schema& s, const std::vector<int>& refs, bool outbound, size_t max) {
    std::vector<std::string> names;
    for (int r : refs) {
        const SchemaRef& ref = s.refs[static_cast<size_t>(r)];
        const std::string& n = s.types[static_cast<size_t>(outbound ? ref.to : ref.from)].name;
        if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    }
    std::string out;
    for (size_t i = 0; i < names.size() && i < max; ++i) out += (i ? ", " : "") + names[i];
    if (names.size() > max) out += ", +" + std::to_string(names.size() - max);
    return out;
}

}  // namespace

CityView::CityView(const Schema& schema, CityStyle style) : city_(schema, style) {}

void CityView::upload(strata::Engine& engine) {
    districtIds_.clear();
    for (size_t d = 0; d < city_.districts().size(); ++d) districtIds_.push_back(uploadMesh(engine, city_.districtMesh(d)));
    roofId_ = uploadMesh(engine, city_.roofMesh(), true);
    glowId_ = 0;
    dirty_ = true;
}

OrbitCamera CityView::home(float aspect) const { return city_.homeCamera(aspect); }

void CityView::select(int type) {
    selected_ = type;
    dirty_ = true;
}

bool CityView::selectName(const std::string& name) {
    const int t = city_.schema().typeByName(name);
    if (t < 0) return false;
    select(t);
    return true;
}

void CityView::pick(const StrataRayHit& hit) {
    for (size_t d = 0; d < districtIds_.size(); ++d)
        if (districtIds_[d] && districtIds_[d] == hit.mesh_id) {
            select(city_.typeAt(d, hit.triangle));
            return;
        }
    if (hit.mesh_id && hit.mesh_id == roofId_) {
        select(tagForTriangle(city_.roofMesh().tags(), hit.triangle));
        return;
    }
    select(-1);
}

bool CityView::key(int32_t sc) {
    if (sc >= kScan1 && sc <= kScan4) {
        setHops(sc - kScan1 + 1);
        return true;
    }
    if (sc == kScanR) {
        recursion_ = !recursion_;
        return true;
    }
    if (sc == kScanU && selected_ >= 0) {  // to the first type that uses this one
        const auto& in = city_.schema().in[static_cast<size_t>(selected_)];
        if (!in.empty()) select(city_.schema().refs[static_cast<size_t>(in.front())].from);
        return true;
    }
    return sc == kScanF;
}

bool CityView::focus(Vec3& target) const {
    if (selected_ < 0) return false;
    target = city_.roof(selected_);
    target.y = 0;
    return true;
}

void CityView::sync(strata::Engine& engine) {
    if (!dirty_) return;
    dirty_ = false;
    if (glowId_) engine.removeMesh(glowId_);
    glowId_ = 0;
    if (selected_ < 0) return;
    // Caps: the selection (gold), what it reaches within the hop depth (cyan,
    // dimmer with distance) and what uses it directly (orange).
    MeshBuilder m;
    auto cap = [&](int t, Rgb c, float lift) {
        const Lot& l = city_.lot(t);
        const float lo[3] = {l.x0 - 0.1f, l.height + lift, l.z0 - 0.1f};
        const float hi[3] = {l.x1 + 0.1f, l.height + lift + 0.18f, l.z1 + 0.1f};
        m.addBox(lo, hi, c, c, -1);
    };
    const Schema& s = city_.schema();
    for (int r : s.in[static_cast<size_t>(selected_)]) cap(s.refs[static_cast<size_t>(r)].from, {1, 0.6f, 0.2f}, 0.3f);
    for (const auto& kv : s.reach(selected_, hops_)) {
        if (kv.first == selected_) continue;
        const float k = 1.0f - 0.18f * static_cast<float>(kv.second - 1);
        cap(kv.first, {0.3f * k, 0.85f * k, 1.0f * k}, 0.3f);
    }
    cap(selected_, {1, 0.82f, 0.25f}, 0.3f);
    glowId_ = uploadMesh(engine, m, true);
}

void CityView::draw(Overlay& o, const CameraMatrices& cam, int w, int h) {
    const Schema& s = city_.schema();
    const float lift = city_.tallest() + 2;
    std::vector<Rect> taken;
    taken.push_back({0, 0, static_cast<float>(w) * 0.6f, 16 + 7 * 20.0f});  // the panel

    // IMPORTS arcs between districts, one per (importer, source) pair, drawn
    // heavier the more references cross it.
    std::map<std::pair<int, int>, int> crossing;
    for (const SchemaRef& r : s.refs) {
        const int a = s.types[static_cast<size_t>(r.from)].module, b = s.types[static_cast<size_t>(r.to)].module;
        if (a != b) crossing[{a, b}]++;
    }
    for (size_t m = 0; m < s.modules.size(); ++m)
        for (const std::string& imp : s.modules[m].imports) {
            const int src = s.moduleByName(imp);
            if (src < 0) continue;
            const Vec3 a = city_.districtCentre(m), b = city_.districtCentre(static_cast<size_t>(src));
            const float d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
            const int count = crossing[{static_cast<int>(m), src}];
            const int strands = 1 + std::min(3, static_cast<int>(std::log2(1.0 + count)));
            for (int k = 0; k < strands; ++k) {
                const float off = (static_cast<float>(k) - static_cast<float>(strands - 1) / 2) * 0.35f;
                addArc(o, {a.x + off, lift, a.z}, {b.x + off, lift, b.z}, d * 0.25f, kImport);
            }
        }

    // Recursion loops: every reference inside a recursive group.
    if (recursion_)
        for (const SchemaRef& r : s.refs) {
            const int ga = s.types[static_cast<size_t>(r.from)].scc, gb = s.types[static_cast<size_t>(r.to)].scc;
            if (ga < 0 || ga != gb) continue;
            const Vec3 a = city_.roof(r.from), b = city_.roof(r.to);
            if (r.from == r.to) {  // a self-loop: a small hoop over the roof
                addArc(o, {a.x - 0.4f, a.y + 0.25f, a.z}, {a.x + 0.4f, a.y + 0.25f, a.z}, 1.6f, kLoop);
                continue;
            }
            const float d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
            addArc(o, {a.x, a.y + 0.25f, a.z}, {b.x, b.y + 0.25f, b.z}, 0.6f + d * 0.3f, kLoop);
        }

    // The selection's references: outbound within the hop depth, inbound one hop.
    if (selected_ >= 0) {
        const std::map<int, int> reached = s.reach(selected_, hops_);
        for (const SchemaRef& r : s.refs) {
            const auto f = reached.find(r.from);
            if (f == reached.end() || f->second >= hops_ || !reached.count(r.to)) continue;
            if (reached.at(r.to) != f->second + 1 && r.to != selected_) continue;
            const Vec3 a = city_.roof(r.from), b = city_.roof(r.to);
            const float d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
            float c[4] = {kOut[0], kOut[1], kOut[2], kOut[3] * (1.0f - 0.2f * static_cast<float>(f->second))};
            addArc(o, {a.x, a.y + 0.5f, a.z}, {b.x, b.y + 0.5f, b.z}, 1 + d * 0.3f, c);
        }
        for (int ri : s.in[static_cast<size_t>(selected_)]) {
            const SchemaRef& r = s.refs[static_cast<size_t>(ri)];
            const Vec3 a = city_.roof(r.from), b = city_.roof(r.to);
            const float d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
            addArc(o, {a.x, a.y + 0.5f, a.z}, {b.x, b.y + 0.5f, b.z}, 1 + d * 0.3f, kIn);
        }
    }

    // District names.
    for (size_t d = 0; d < city_.districts().size(); ++d) {
        const District& di = city_.districts()[d];
        float x, y;
        if (!toScreen(cam, {(di.x0 + di.x1) / 2, 0, di.z1 + 1.5f}, w, h, x, y)) continue;
        const std::string& name = s.modules[static_cast<size_t>(di.module)].name;
        const std::string t = name + "  (" + std::to_string(di.types.size()) + ")";
        const float tw = 16.0f * static_cast<float>(t.size());
        const Rect r{x - tw / 2, y, x + tw / 2, y + 16};
        if (overlaps(taken, r)) continue;
        o.text(r.x0, r.y0, 16, t, kInk);
        taken.push_back(r);
    }

    // Building names: the selection and its neighbours first, then the
    // biggest buildings, while they fit.
    std::vector<int> order;
    if (selected_ >= 0) {
        order.push_back(selected_);
        for (const auto& kv : s.reach(selected_, hops_))
            if (kv.first != selected_) order.push_back(kv.first);
        for (int r : s.in[static_cast<size_t>(selected_)]) order.push_back(s.refs[static_cast<size_t>(r)].from);
    }
    std::vector<int> big;
    for (const SchemaType& t : s.types)
        if (!t.flat()) big.push_back(t.id);
    std::stable_sort(big.begin(), big.end(), [&](int a, int b) {
        return s.types[static_cast<size_t>(a)].members > s.types[static_cast<size_t>(b)].members;
    });
    order.insert(order.end(), big.begin(), big.end());
    int placed = 0;
    std::vector<bool> done(s.types.size(), false);
    for (int t : order) {
        if (done[static_cast<size_t>(t)] || placed > 60) continue;
        done[static_cast<size_t>(t)] = true;
        const Vec3 p = city_.roof(t);
        float x, y;
        if (!toScreen(cam, {p.x, p.y + 0.6f, p.z}, w, h, x, y)) continue;
        const bool sel = t == selected_;
        const float size = sel ? 16.0f : 8.0f;
        const std::string& name = s.types[static_cast<size_t>(t)].name;
        const float tw = size * static_cast<float>(name.size());
        const Rect r{x - tw / 2, y - size - 2, x + tw / 2, y - 2};
        if (!sel && (overlaps(taken, r) || r.x0 < 0 || r.x1 > static_cast<float>(w))) continue;
        o.text(r.x0, r.y0, size, name, sel ? kIn : kInk);
        taken.push_back(r);
        ++placed;
    }

    // The panel.
    float py = 16;
    auto line = [&](const std::string& t, const float* c, float size = 16) {
        o.text(16, py, size, t, c);
        py += size + 4;
    };
    if (selected_ >= 0) {
        const SchemaType& t = s.types[static_cast<size_t>(selected_)];
        const SchemaModule& m = s.modules[static_cast<size_t>(t.module)];
        line(t.name + " ::= " + t.kind, kInk);
        line(m.name + "   " + m.file + ":" + std::to_string(t.line) + "   " + std::to_string(t.members) + " members",
             kDim);
        const auto& out = s.out[static_cast<size_t>(selected_)];
        const auto& in = s.in[static_cast<size_t>(selected_)];
        line("uses (" + std::to_string(out.size()) + "): " + joinNames(s, out, true, 6), kOut, 8);
        line("used by (" + std::to_string(in.size()) + "): " + joinNames(s, in, false, 6), kIn, 8);
        const size_t reach = s.reach(selected_, hops_).size() - 1;
        line("within " + std::to_string(hops_) + (hops_ == 1 ? " hop: " : " hops: ") + std::to_string(reach) +
                 " types (keys 1-4)",
             kDim, 8);
        if (t.scc >= 0)
            line("recursive: one of " + std::to_string(s.sccs[static_cast<size_t>(t.scc)].size()) +
                     " types in a cycle",
                 kLoop, 8);
    } else {
        size_t types = 0;
        for (const SchemaType& t : s.types) types += t.flat() ? 0 : 1;
        line(std::to_string(s.modules.size()) + " modules, " + std::to_string(types) + " types, " +
                 std::to_string(s.refs.size()) + " references, " + std::to_string(s.sccs.size()) + " recursive groups",
             kInk);
        line("click a building to see its definition and what it uses", kDim);
    }

    if (compact_) return;
    float ly = static_cast<float>(h) - 16 - 12.0f * static_cast<float>(cityLegend().size() + 3);
    for (const auto& k : cityLegend()) {
        const Rgb c = typeKindColor(k.second);
        const float rgba[4] = {c.r, c.g, c.b, 1};
        o.text(16, ly, 8, "##", rgba, false);
        o.text(36, ly, 8, k.first, kDim);
        ly += 12;
    }
    o.text(16, ly, 8, "##", kLoop, false);
    o.text(36, ly, 8, "recursive (glowing roof, red loops)", kDim);
    ly += 12;
    o.text(16, ly, 8, "height: log of member count   arcs: IMPORTS; cyan uses, orange used by", kDim);
    ly += 12;
    o.text(16, ly, 8, "drag orbit  right-drag pan  wheel zoom  click pick  1-4 hops  U user  R loops  F focus  H home",
           kDim);
}

}  // namespace asn1viz
