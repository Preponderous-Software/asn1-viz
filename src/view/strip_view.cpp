#include "view/strip_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace asn1viz {

namespace {

// SDL3 scancodes.
constexpr int32_t kScanU = 24, kScanF = 9, kScanL = 15, kScanN = 17, kScanP = 19, kScanRight = 79, kScanLeft = 80;

const float kInk[4] = {0.92f, 0.94f, 0.97f, 1};
const float kDim[4] = {0.62f, 0.66f, 0.74f, 1};
const float kSegLine[4] = {1, 1, 1, 1};
const float kLink[4] = {1, 0.82f, 0.3f, 0.9f};
const float kTick[4] = {0.5f, 0.55f, 0.65f, 0.35f};

std::string withCommas(int64_t v) {
    std::string s = std::to_string(v), out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0) out += ',';
        out += s[i];
    }
    return out;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
    return s;
}

// The last component of a path, for a short label.
std::string leaf(const std::string& path) {
    const size_t dot = path.rfind('.');
    return dot == std::string::npos ? path : path.substr(dot + 1);
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

StripView::StripView(const Layout& layout, StripStyle style) : strips_(layout, style) {
    // Byte boundaries: a faint tick across each row every 8 bits.
    for (size_t l = 0; l < strips_.laneCount(); ++l) {
        const int64_t bits = strips_.layout().lanes[l].bits;
        for (int64_t b = 0; b <= bits; b += 8) {
            if (b % style.rowBits == 0 && b != 0 && b != bits) continue;  // the row's own edge
            const BitPlace p = strips_.place(l, b == bits ? b - 1 : b);
            const float x = b == bits ? p.x1 : p.x0;
            StrataLineVertex v{};
            std::copy(kTick, kTick + 4, v.color);
            v.position[0] = x;
            v.position[1] = 0.02f;
            v.position[2] = p.z0;
            byteTicks_.push_back(v);
            v.position[2] = p.z1;
            byteTicks_.push_back(v);
        }
    }
}

void StripView::sync(strata::Engine& engine) {
    if (!glowDirty_) return;
    glowDirty_ = false;
    if (glowId_) engine.removeMesh(glowId_);
    glowId_ = 0;
    if (selLane_ < 0) return;
    // A glowing cap on every bit of the selected field (and of the same field
    // in the other lanes), brighter on the selected segment.
    MeshBuilder m;
    const Layout& L = strips_.layout();
    auto cap = [&](size_t lane, int64_t start, int64_t len, Rgb c) {
        const LayoutLane& ln = L.lanes[lane];
        for (int64_t b = start; b < start + len; ++b) {
            const int s = ln.segmentAt(b);
            if (s < 0) continue;
            const float top = strips_.segmentHeight(lane, s) + (ln.bit(b) == 1 ? strips_.style().oneBonus : 0.0f);
            const BitPlace p = strips_.place(lane, b);
            const float lo[3] = {p.x0 + 0.03f, top, p.z0 + 0.05f}, hi[3] = {p.x1 - 0.03f, top + 0.12f, p.z1 - 0.05f};
            m.addBox(lo, hi, c, c, -1);
        }
    };
    const size_t sl = static_cast<size_t>(selLane_);
    const LayoutFrame& f = L.lanes[sl].frames[static_cast<size_t>(selFrame_)];
    const Rgb frameGlow{1.0f, 0.78f, 0.22f}, segGlow{1, 1, 1};
    cap(sl, f.start, f.end - f.start, frameGlow);
    if (!wholeFrame_ && selSegment_ >= 0) {
        const LayoutSegment& s = L.lanes[sl].segments[static_cast<size_t>(selSegment_)];
        const float lift = 0.13f;
        for (int64_t b = s.start; b < s.start + s.len; ++b) {
            const float top = strips_.segmentHeight(sl, selSegment_) + (L.lanes[sl].bit(b) == 1 ? strips_.style().oneBonus : 0.0f) + lift;
            const BitPlace p = strips_.place(sl, b);
            const float lo[3] = {p.x0 + 0.03f, top, p.z0 + 0.05f}, hi[3] = {p.x1 - 0.03f, top + 0.12f, p.z1 - 0.05f};
            m.addBox(lo, hi, segGlow, segGlow, -1);
        }
    }
    for (size_t other = 0; other < strips_.laneCount(); ++other) {
        if (other == sl) continue;
        const int of = strips_.matchingFrame(sl, selFrame_, other);
        if (of < 0) continue;
        const LayoutFrame& g = L.lanes[other].frames[static_cast<size_t>(of)];
        cap(other, g.start, g.end - g.start, frameGlow);
    }
    glowId_ = uploadMesh(engine, m, true);
}

void StripView::upload(strata::Engine& engine) {
    glowId_ = 0;
    glowDirty_ = true;
    meshIds_.clear();
    for (size_t l = 0; l < strips_.laneCount(); ++l) meshIds_.push_back(uploadMesh(engine, strips_.laneMesh(l)));
}

OrbitCamera StripView::home(float aspect) const { return strips_.homeCamera(aspect); }

void StripView::select(int lane, int segment, int frame, bool wholeFrame) {
    glowDirty_ = true;
    selLane_ = lane;
    selSegment_ = segment;
    selFrame_ = frame;
    wholeFrame_ = wholeFrame;
}

void StripView::pick(const StrataRayHit& hit) {
    for (size_t l = 0; l < meshIds_.size(); ++l) {
        if (meshIds_[l] == 0 || meshIds_[l] != hit.mesh_id) continue;
        const int seg = tagForTriangle(strips_.laneMesh(l).tags(), hit.triangle);
        if (seg < 0) break;
        select(static_cast<int>(l), seg, strips_.layout().lanes[l].segments[static_cast<size_t>(seg)].frame, false);
        return;
    }
    select(-1, -1, -1, false);
}

bool StripView::selectPath(const std::string& path, size_t lane) {
    if (lane >= strips_.laneCount()) return false;
    const LayoutLane& l = strips_.layout().lanes[lane];
    const int f = l.frameByPath(path);
    if (f < 0) return false;
    select(static_cast<int>(lane), l.segmentAt(l.frames[static_cast<size_t>(f)].start), f, true);
    return true;
}

std::string StripView::selectionPath() const {
    if (selLane_ < 0) return "";
    return strips_.layout().lanes[static_cast<size_t>(selLane_)].frames[static_cast<size_t>(selFrame_)].path;
}

bool StripView::key(int32_t sc) {
    if (sc == kScanL) {
        linkDepth_ = (linkDepth_ + 1) % 4;
        return true;
    }
    if (selLane_ < 0) return false;
    const LayoutLane& l = strips_.layout().lanes[static_cast<size_t>(selLane_)];
    if (sc == kScanU) {
        const int parent = l.frames[static_cast<size_t>(selFrame_)].parent;
        if (wholeFrame_ && parent >= 0) selFrame_ = parent;
        wholeFrame_ = true;
        glowDirty_ = true;
        return true;
    }
    if (sc == kScanN || sc == kScanRight || sc == kScanP || sc == kScanLeft) {
        const int step = (sc == kScanN || sc == kScanRight) ? 1 : -1;
        const int n = static_cast<int>(l.segments.size());
        const int s = std::clamp(selSegment_ + step, 0, n - 1);
        select(selLane_, s, l.segments[static_cast<size_t>(s)].frame, false);
        return true;
    }
    if (sc == kScanF) return true;  // the shell refocuses on any handled key (see focus())
    return false;
}

bool StripView::focus(Vec3& target) const {
    if (selLane_ < 0) return false;
    const LayoutLane& l = strips_.layout().lanes[static_cast<size_t>(selLane_)];
    const LayoutFrame& f = l.frames[static_cast<size_t>(selFrame_)];
    target = strips_.anchor(static_cast<size_t>(selLane_), wholeFrame_ ? f.start : l.segments[static_cast<size_t>(selSegment_)].start, 0);
    return true;
}

void StripView::outline(Overlay& o, size_t lane, int64_t start, int64_t len, float h, const float c[4]) const {
    for (const BitPlace& p : strips_.rowSpans(lane, start, len)) {
        const float x0 = p.x0 + 0.02f, x1 = p.x1 - 0.02f, z0 = p.z0 + 0.04f, z1 = p.z1 - 0.04f;
        const Vec3 a{x0, h, z0}, b{x1, h, z0}, cc{x1, h, z1}, d{x0, h, z1};
        const Vec3 a0{x0, 0.01f, z0}, b0{x1, 0.01f, z0}, c0{x1, 0.01f, z1}, d0{x0, 0.01f, z1};
        o.line(a, b, c);
        o.line(b, cc, c);
        o.line(cc, d, c);
        o.line(d, a, c);
        o.line(a0, b0, c);
        o.line(b0, c0, c);
        o.line(c0, d0, c);
        o.line(d0, a0, c);
        o.line(a, a0, c);
        o.line(b, b0, c);
        o.line(cc, c0, c);
        o.line(d, d0, c);
    }
}

void StripView::draw(Overlay& o, const CameraMatrices& cam, int w, int h) {
    o.lines.insert(o.lines.end(), byteTicks_.begin(), byteTicks_.end());
    const Layout& L = strips_.layout();
    std::vector<Rect> taken;
    const float panelBottom = 16 + 8 * 20.0f;
    taken.push_back({0, 0, static_cast<float>(w) * 0.55f, panelBottom});  // the pick panel

    // Lane titles above each lane.
    for (size_t l = 0; l < strips_.laneCount(); ++l) {
        const LayoutLane& lane = L.lanes[l];
        float x, y;
        const Vec3 p{strips_.laneX0(l) + strips_.laneWidth() / 2, 0, -1.5f};
        if (!toScreen(cam, p, w, h, x, y)) continue;
        std::string t = upper(lane.codec) + "  " + withCommas(lane.encodedBits) + " bits";
        if (!lane.source.empty()) t += "  (" + lane.source + ")";
        const float tw = 16.0f * static_cast<float>(t.size());
        const Rect r{x - tw / 2, y - 24, x + tw / 2, y - 8};
        if (!overlaps(taken, r)) {
            o.text(r.x0, r.y0, 16, t, kInk);
            taken.push_back(r);
        }
    }

    // Selection: the segment, its frame, and the same frame in the other lanes.
    if (selLane_ >= 0) {
        const size_t sl = static_cast<size_t>(selLane_);
        const LayoutLane& lane = L.lanes[sl];
        const LayoutFrame& f = lane.frames[static_cast<size_t>(selFrame_)];
        const float fh = strips_.frameHeight(sl, selFrame_) + 0.4f;
        if (!wholeFrame_ && selSegment_ >= 0) {
            const LayoutSegment& s = lane.segments[static_cast<size_t>(selSegment_)];
            outline(o, sl, s.start, s.len, strips_.segmentHeight(sl, selSegment_) + 0.5f, kSegLine);
        }
        for (size_t other = 0; other < strips_.laneCount(); ++other) {
            if (other == sl) continue;
            const int of = strips_.matchingFrame(sl, selFrame_, other);
            if (of < 0) continue;
            const LayoutFrame& g = L.lanes[other].frames[static_cast<size_t>(of)];
            const Vec3 a = strips_.anchor(sl, f.start, fh + 0.2f);
            const Vec3 b = strips_.anchor(other, g.start, strips_.frameHeight(other, of) + 0.6f);
            const Vec3 mid{(a.x + b.x) / 2, std::max(a.y, b.y) + 6, (a.z + b.z) / 2};
            o.line(a, mid, kLink);
            o.line(mid, b, kLink);
        }
        // A label at the picked field.
        float x, y;
        if (toScreen(cam, strips_.anchor(sl, f.start, fh + 0.4f), w, h, x, y)) {
            const std::string t = leaf(f.path);
            const Rect r{x, y - 22, x + 16.0f * static_cast<float>(t.size()), y - 6};
            o.text(r.x0, r.y0, 16, t, kLink);
            taken.push_back(r);
        }
    }

    // Links for a whole tree depth (L), from the first lane to the others.
    if (linkDepth_ > 0 && strips_.laneCount() > 1) {
        const LayoutLane& first = L.lanes[0];
        for (size_t fi = 0; fi < first.frames.size(); ++fi) {
            if (first.frames[fi].depth != linkDepth_) continue;
            for (size_t other = 1; other < strips_.laneCount(); ++other) {
                const int of = strips_.matchingFrame(0, static_cast<int>(fi), other);
                if (of < 0) continue;
                const Vec3 a = strips_.anchor(0, first.frames[fi].start, strips_.frameHeight(0, static_cast<int>(fi)) + 0.3f);
                const Vec3 b = strips_.anchor(other, L.lanes[other].frames[static_cast<size_t>(of)].start,
                                              strips_.frameHeight(other, of) + 0.3f);
                const float c[4] = {kLink[0], kLink[1], kLink[2], 0.45f};
                o.line(a, b, c);
            }
        }
    }

    // Field names at the start of the top-level fields, where they fit.
    for (int depth = 1; depth <= 3; ++depth)
        for (size_t l = 0; l < strips_.laneCount(); ++l) {
            const LayoutLane& lane = L.lanes[l];
            for (size_t fi = 0; fi < lane.frames.size(); ++fi) {
                const LayoutFrame& f = lane.frames[fi];
                if (f.depth != depth || f.end - f.start < 8) continue;
                float x, y;
                if (!toScreen(cam, strips_.anchor(l, f.start, strips_.frameHeight(l, static_cast<int>(fi)) + 0.3f), w,
                              h, x, y))
                    continue;
                const std::string t = leaf(f.path);
                const float size = depth == 1 ? 16.0f : 8.0f;
                const Rect r{x, y - size - 4, x + size * static_cast<float>(t.size()), y - 2};
                if (r.x1 > static_cast<float>(w) || overlaps(taken, r)) continue;
                o.text(r.x0, r.y0, size, t, depth == 1 ? kInk : kDim);
                taken.push_back(r);
            }
        }

    // The pick panel.
    float py = 16;
    const size_t fit = static_cast<size_t>(std::max(8, (w - 16 - 7 * 16) / 16));
    auto line = [&](const std::string& s, const float* c, float size = 16) {
        o.text(16, py, size, s, c);
        py += size + 4;
    };
    if (selLane_ >= 0) {
        const size_t sl = static_cast<size_t>(selLane_);
        const LayoutLane& lane = L.lanes[sl];
        const LayoutFrame& f = lane.frames[static_cast<size_t>(selFrame_)];
        line(f.path, kInk);
        std::string typeLine = upper(lane.codec) + "  type " + f.type;
        const auto at = where_.find(f.type);
        if (at != where_.end()) typeLine += "  (" + at->second + ")";
        line(typeLine, kDim);
        if (wholeFrame_) {
            line("field  bits [" + withCommas(f.start) + ", " + withCommas(f.end) + ")  " +
                     withCommas(f.end - f.start) + " bits",
                 kInk);
            line("bits   " + lane.bitString(f.start, f.end - f.start, fit - 3), kDim);
            line("bytes  " + lane.hexOfBits(f.start, f.end - f.start, fit), kDim);
        } else {
            const LayoutSegment& s = lane.segments[static_cast<size_t>(selSegment_)];
            std::string k = s.kind + (s.inferred ? "  (inferred)" : "  (exact)");
            if (!s.member.empty()) k += "  -> " + s.member;
            line(k, kInk);
            line("bits [" + withCommas(s.start) + ", " + withCommas(s.start + s.len) + ")  " + withCommas(s.len) +
                     (s.len == 1 ? " bit" : " bits") + "  in a field of " + withCommas(f.end - f.start),
                 kInk);
            line("bits   " + lane.bitString(s.start, s.len, fit - 3), kDim);
            line("bytes  " + lane.hexOfBits(s.start, s.len, fit), kDim);
        }
    } else {
        line(L.pdu + "  from " + L.valueSource, kInk);
        line("click a block to see its field, its kind and its bits", kDim);
    }

    if (compact_) return;
    // The legend, bottom left.
    float ly = static_cast<float>(h) - 16 - 12.0f * static_cast<float>(legendKinds().size() + 2);
    for (const auto& k : legendKinds()) {
        const Rgb c = kindColor(k.second, "");
        const float rgba[4] = {c.r, c.g, c.b, 1};
        o.text(16, ly, 8, "##", rgba, false);
        o.text(36, ly, 8, k.first, kDim);
        ly += 12;
    }
    o.text(16, ly, 8, "values: by ASN.1 type   dark top: inferred label   tall: a 1 bit", kDim);
    ly += 12;
    o.text(16, ly, 8, "drag orbit  right-drag pan  wheel zoom  click pick  U parent  N/P step  F focus  L links  H home",
           kDim);
}

}  // namespace asn1viz
