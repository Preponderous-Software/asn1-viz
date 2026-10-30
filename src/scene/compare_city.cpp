#include "scene/compare_city.h"

#include <cmath>

namespace asn1viz {

namespace {

Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
Rgb scale(Rgb c, float k) { return {std::min(1.0f, c.r * k), std::min(1.0f, c.g * k), std::min(1.0f, c.b * k)}; }

// One floor's two parts: the lines both runs wrote, then the difference.
struct Part {
    float h;
    Rgb c;
};

}  // namespace

// Okabe and Ito's blue and orange, lifted a little for a dark ground: apart
// in hue and in lightness, so they read for deuteranopes and protanopes, and
// clear of the selection's gold and the fixer's pink.
Rgb growthColor() { return {0.34f, 0.64f, 0.98f}; }
Rgb shrinkColor() { return {0.96f, 0.5f, 0.12f}; }
Rgb fixerDiffColor() { return {1.0f, 0.3f, 0.6f}; }
Rgb netColor(int delta) { return delta > 0 ? growthColor() : delta < 0 ? shrinkColor() : Rgb{0.85f, 0.87f, 0.9f}; }

Rgb sharedColor(int fn, int functionCount, bool changedType) {
    const Rgb grey{0.36f, 0.38f, 0.44f};
    if (!changedType) return scale(mix(functionColor(fn, functionCount), grey, 0.85f), 0.55f);
    return scale(mix(functionColor(fn, functionCount), grey, 0.7f), 1.0f);
}

CompareCity::CompareCity(const Schema& schema, const CompileCompare& compare, CityStyle style)
    : city_(schema, style), compare_(compare) {
    int most = 1;
    for (int t = 0; t < compare.typeCount(); ++t) {
        const CompareEntry& e = compare.type(t);
        int sum = 0;
        for (size_t i = 0; i < e.a.size(); ++i) sum += std::max(e.a[i], e.b[i]);
        most = std::max(most, sum);
    }
    // As the replay: the biggest type stands about twice as tall as the city's tallest building.
    tallestStack_ = std::max(6.0f, city_.tallest() * 2.2f);
    lineScale_ = tallestStack_ / static_cast<float>(most);
}

float CompareCity::height(int type) const {
    const CompareEntry& e = compare_.type(type);
    float h = 0;
    for (size_t i = 0; i < e.a.size(); ++i) {
        const int common = std::min(e.a[i], e.b[i]), d = std::abs(e.b[i] - e.a[i]);
        if (common > 0) h += std::max(0.02f, static_cast<float>(common) * lineScale_);
        if (d > 0) h += std::max(0.02f, static_cast<float>(d) * lineScale_);
    }
    return h > 0 ? h : city_.lot(type).height;
}

void CompareCity::buildDistrict(size_t di, MeshBuilder& mesh) const {
    const District& d = city_.districts()[di];
    mesh.addTile(d.x0, d.z0, d.x1, d.z1, 0.0f, {30.0f / 255, 34.0f / 255, 46.0f / 255}, -1);
    const int fns = static_cast<int>(compare_.functions().size());
    for (int t : d.types) {
        const Lot& lot = city_.lot(t);
        const CompareEntry& e = compare_.type(t);
        if (e.linesA == 0 && e.linesB == 0) {
            // No code in either run (a value or a class): a dim block.
            const Rgb side = scale(mix(typeKindColor(schema().types[static_cast<size_t>(t)].kind), {0.3f, 0.32f, 0.38f}, 0.7f), 0.8f);
            const float lo[3] = {lot.x0, 0.0f, lot.z0}, hi[3] = {lot.x1, lot.height, lot.z1};
            mesh.addBox(lo, hi, side, scale(side, 1.15f), t);
            continue;
        }
        const bool changed = e.changed();
        float y = 0;
        for (int k = -1; k < fns; ++k) {
            const int fn = k < 0 ? fns : k;
            const int a = e.a[static_cast<size_t>(fn)], b = e.b[static_cast<size_t>(fn)];
            const int common = std::min(a, b), diff = std::abs(b - a);
            const Part parts[2] = {
                {common > 0 ? std::max(0.02f, static_cast<float>(common) * lineScale_) : 0, sharedColor(fn, fns, changed)},
                {diff > 0 ? std::max(0.02f, static_cast<float>(diff) * lineScale_) : 0, b > a ? growthColor() : shrinkColor()}};
            for (const Part& p : parts) {
                if (p.h <= 0) continue;
                const float lo[3] = {lot.x0, y, lot.z0}, hi[3] = {lot.x1, y + p.h, lot.z1};
                mesh.addBox(lo, hi, p.c, scale(p.c, 1.12f), bandTag(t, fn));
                y += p.h;
            }
        }
    }
}

void CompareCity::buildMarks(const CompilePick& sel, MeshBuilder& m) const {
    int32_t tag = -1;
    auto cap = [&](float x0, float z0, float x1, float z1, float y, float h, float grow, Rgb c) {
        const float lo[3] = {x0 - grow, y, z0 - grow}, hi[3] = {x1 + grow, y + h, z1 + grow};
        m.addBox(lo, hi, c, c, tag);
    };
    for (int t = 0; t < compare_.typeCount(); ++t) {
        const CompareEntry& e = compare_.type(t);
        const Lot& l = city_.lot(t);
        tag = t;
        // A changed type's roof, in the colour of its net change, so the few
        // changed buildings stand out however thin their changed floors are.
        if (e.changed()) cap(l.x0, l.z0, l.x1, l.z1, height(t), 0.12f, 0.06f, netColor(e.delta()));
        if (e.fixerDiffers())
            cap(l.x0 + 0.22f, l.z0 + 0.22f, l.x1 - 0.22f, l.z1 - 0.22f, height(t) + (e.changed() ? 0.12f : 0), 0.14f, 0,
                fixerDiffColor());
    }
    if ((sel.kind != CompilePick::Type && sel.kind != CompilePick::Band) || sel.type < 0) return;
    const Rgb gold{1, 0.82f, 0.25f};
    const Lot& l = city_.lot(sel.type);
    tag = sel.kind == CompilePick::Band ? bandTag(sel.type, sel.fn) : sel.type;
    if (sel.kind == CompilePick::Band) {
        // Two thin rings, under and over the floor (both its parts).
        const CompareEntry& e = compare_.type(sel.type);
        const int fns = static_cast<int>(compare_.functions().size());
        float y = 0;
        for (int k = -1; k < fns; ++k) {
            const int fn = k < 0 ? fns : k;
            const int a = e.a[static_cast<size_t>(fn)], b = e.b[static_cast<size_t>(fn)];
            const int common = std::min(a, b), diff = std::abs(b - a);
            float h = 0;
            if (common > 0) h += std::max(0.02f, static_cast<float>(common) * lineScale_);
            if (diff > 0) h += std::max(0.02f, static_cast<float>(diff) * lineScale_);
            if (fn == sel.fn && h > 0) {
                cap(l.x0, l.z0, l.x1, l.z1, y - 0.03f, 0.06f, 0.12f, gold);
                cap(l.x0, l.z0, l.x1, l.z1, y + h - 0.03f, 0.06f, 0.12f, gold);
            }
            y += h;
        }
    }
    cap(l.x0, l.z0, l.x1, l.z1, height(sel.type) + 0.75f, 0.16f, 0.1f, gold);
}

OrbitCamera CompareCity::homeCamera(float aspect) const {
    float x0 = 1e9f, z0 = 1e9f, x1 = -1e9f, z1 = -1e9f;
    for (const District& d : city_.districts())
        x0 = std::min(x0, d.x0), z0 = std::min(z0, d.z0), x1 = std::max(x1, d.x1), z1 = std::max(z1, d.z1);
    if (city_.districts().empty()) x0 = z0 = x1 = z1 = 0;
    OrbitCamera c;
    c.target = {(x0 + x1) / 2, 0, (z0 + z1) / 2};
    c.yawDeg = -24;
    c.pitchDeg = 38;
    const float w = x1 - x0, dd = z1 - z0;
    const float half = std::max(w / 2 / std::max(aspect, 0.3f), dd / 2) * 1.2f + tallest() * 0.6f;
    c.distance = half / std::tan(c.fovYDeg * 3.14159265f / 360) + 6;
    c.farPlane = c.distance * 4 + 500;
    return c;
}

}  // namespace asn1viz
