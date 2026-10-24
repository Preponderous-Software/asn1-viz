#include "scene/strip.h"

#include <algorithm>
#include <cmath>

namespace asn1viz {

namespace {

Rgb rgb(int r, int g, int b) { return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                                       static_cast<float>(b) / 255.0f}; }

bool contains(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

// Values by ASN.1 type family.
Rgb valueColor(const std::string& type) {
    if (contains(type, "INTEGER") || contains(type, "Version") || contains(type, "SerialNumber") ||
        contains(type, "ENUMERATED") || contains(type, "REAL"))
        return rgb(76, 146, 222);
    if (contains(type, "OBJECT IDENTIFIER") || contains(type, "AttributeType") || contains(type, "OID"))
        return rgb(158, 118, 230);
    if (contains(type, "Time") || contains(type, "Date")) return rgb(232, 196, 92);
    if (contains(type, "BIT STRING") || contains(type, "UniqueIdentifier")) return rgb(64, 186, 196);
    if (contains(type, "OCTET STRING") || contains(type, "ANY") || contains(type, "AttributeValue"))
        return rgb(86, 172, 150);
    if (contains(type, "String")) return rgb(120, 200, 120);
    if (contains(type, "BOOLEAN") || contains(type, "NULL")) return rgb(222, 140, 170);
    return rgb(110, 150, 200);
}

}  // namespace

Rgb kindColor(const std::string& kind, const std::string& type) {
    if (kind == "value") return valueColor(type);
    if (kind == "presence-bit" || kind == "presence-bitmap") return rgb(250, 214, 64);
    if (kind == "extension-bit") return rgb(250, 128, 40);
    if (kind == "extension-bitmap") return rgb(250, 160, 70);
    if (kind == "extension-bitmap-length") return rgb(214, 120, 54);
    if (kind == "choice-index") return rgb(236, 92, 214);
    if (kind == "length") return rgb(246, 246, 246);
    if (kind == "count") return rgb(206, 226, 246);
    if (kind == "normally-small-length" || kind == "normally-small-number") return rgb(226, 236, 200);
    if (kind == "alignment") return rgb(72, 74, 84);
    if (kind == "open-type-length") return rgb(240, 84, 110);
    if (kind == "open-type-padding") return rgb(120, 54, 70);
    if (kind == "trailing-padding") return rgb(52, 54, 62);
    if (kind == "open-type-unknown") return rgb(180, 70, 90);
    if (kind == "undecoded") return rgb(150, 20, 20);
    return rgb(255, 0, 255);
}

const std::vector<std::pair<std::string, std::string>>& legendKinds() {
    static const std::vector<std::pair<std::string, std::string>> k = {
        {"presence bit", "presence-bit"},       {"extension bit", "extension-bit"},
        {"choice index", "choice-index"},       {"length", "length"},
        {"count", "count"},                     {"alignment (APER)", "alignment"},
        {"open-type length", "open-type-length"}, {"trailing padding", "trailing-padding"},
    };
    return k;
}

StripScene::StripScene(const Layout& layout, StripStyle style) : layout_(layout), style_(style) {
    meshes_.resize(layout_.lanes.size());
    for (size_t l = 0; l < layout_.lanes.size(); ++l) {
        const LayoutLane& lane = layout_.lanes[l];
        MeshBuilder& m = meshes_[l];
        for (size_t s = 0; s < lane.segments.size(); ++s) {
            const LayoutSegment& seg = lane.segments[s];
            const LayoutFrame& fr = lane.frames[static_cast<size_t>(seg.frame)];
            Rgb side = kindColor(seg.kind, fr.type);
            Rgb top = side;
            if (seg.inferred) top = {side.r * 0.45f, side.g * 0.45f, side.b * 0.45f};
            const float h = segmentHeight(l, static_cast<int>(s));
            for (int64_t b = seg.start; b < seg.start + seg.len; ++b) {
                const BitPlace p = place(l, b);
                const int v = lane.bit(b);
                const float shade = v == 1 ? 1.0f : 0.62f;
                const Rgb sd{side.r * shade, side.g * shade, side.b * shade};
                const Rgb tp{top.r * shade, top.g * shade, top.b * shade};
                const float lo[3] = {p.x0 + 0.06f, 0, p.z0 + 0.08f};
                const float hi[3] = {p.x1 - 0.06f, h + (v == 1 ? style_.oneBonus : 0.0f), p.z1 - 0.08f};
                m.addBox(lo, hi, sd, tp, static_cast<int32_t>(s));
            }
        }
    }
}

float StripScene::laneX0(size_t lane) const {
    return static_cast<float>(lane) * (laneWidth() + style_.laneGap);
}

float StripScene::laneDepth(size_t lane) const {
    const int64_t rows = (layout_.lanes[lane].bits + style_.rowBits - 1) / style_.rowBits;
    return static_cast<float>(rows) * style_.rowPitch;
}

BitPlace StripScene::place(size_t lane, int64_t bit) const {
    const int64_t row = bit / style_.rowBits, col = bit % style_.rowBits;
    const float x0 = laneX0(lane) + static_cast<float>(col) * style_.bitPitch;
    const float z0 = static_cast<float>(row) * style_.rowPitch;
    return {x0, x0 + style_.bitPitch, z0, z0 + style_.rowPitch};
}

float StripScene::frameHeight(size_t lane, int frame) const {
    const LayoutLane& l = layout_.lanes[lane];
    const int depth = frame >= 0 && frame < static_cast<int>(l.frames.size()) ? l.frames[frame].depth : 0;
    return style_.baseHeight + style_.depthStep * static_cast<float>(depth);
}

float StripScene::segmentHeight(size_t lane, int segment) const {
    return frameHeight(lane, layout_.lanes[lane].segments[static_cast<size_t>(segment)].frame);
}

Vec3 StripScene::anchor(size_t lane, int64_t bit, float height) const {
    const BitPlace p = place(lane, bit);
    return {(p.x0 + p.x1) / 2, height, (p.z0 + p.z1) / 2};
}

std::vector<BitPlace> StripScene::rowSpans(size_t lane, int64_t start, int64_t len) const {
    std::vector<BitPlace> out;
    int64_t b = start;
    const int64_t end = start + std::max<int64_t>(len, 1);
    while (b < end) {
        const int64_t rowEnd = (b / style_.rowBits + 1) * style_.rowBits;
        const int64_t e = std::min(end, rowEnd);
        const BitPlace a = place(lane, b), z = place(lane, e - 1);
        out.push_back({a.x0, z.x1, a.z0, a.z1});
        b = e;
    }
    return out;
}

int StripScene::matchingFrame(size_t fromLane, int frame, size_t toLane) const {
    if (fromLane >= layout_.lanes.size() || toLane >= layout_.lanes.size()) return -1;
    const LayoutLane& from = layout_.lanes[fromLane];
    if (frame < 0 || frame >= static_cast<int>(from.frames.size())) return -1;
    return layout_.lanes[toLane].frameByPath(from.frames[static_cast<size_t>(frame)].path);
}

OrbitCamera StripScene::homeCamera(float aspect) const {
    OrbitCamera c;
    const size_t n = std::max<size_t>(1, laneCount());
    float depth = 0;
    for (size_t l = 0; l < laneCount(); ++l) depth = std::max(depth, laneDepth(l));
    const float width = static_cast<float>(n) * laneWidth() + static_cast<float>(n - 1) * style_.laneGap;
    c.target = {width / 2, 0, depth / 2};
    c.yawDeg = 0;
    const bool shallow = depth < width * 0.2f;  // a short encoding: look across it, not down on it
    c.pitchDeg = shallow ? 40.0f : 64.0f;
    const float half = std::max(width / 2 / std::max(aspect, 0.3f), depth / 2) * (shallow ? 1.25f : 1.08f);
    c.distance = half / std::tan(c.fovYDeg * 3.14159265f / 360) + 4;
    c.farPlane = c.distance * 4 + 500;
    return c;
}

}  // namespace asn1viz
