#include "scene/city.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace asn1viz {

namespace {

Rgb rgb(int r, int g, int b) {
    return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f};
}

bool has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

// Display order of kinds within a district (similar types cluster).
int kindRank(const std::string& k) {
    static const char* order[] = {"SEQUENCE", "SET", "CHOICE", "SEQUENCE OF", "SET OF", "ENUMERATED", "INTEGER",
                                  "BOOLEAN", "BIT STRING", "OCTET STRING", "OBJECT IDENTIFIER", "reference", "class"};
    for (int i = 0; i < static_cast<int>(sizeof order / sizeof order[0]); ++i)
        if (k == order[i]) return i;
    if (has(k, "String") || has(k, "Time")) return 20;
    if (k == "value" || k == "value set" || k == "object" || k == "object field") return 40;
    return 30;
}

}  // namespace

Rgb typeKindColor(const std::string& k) {
    if (k == "SEQUENCE" || k == "SET") return rgb(84, 150, 230);
    if (k == "CHOICE") return rgb(220, 100, 210);
    if (k == "SEQUENCE OF" || k == "SET OF") return rgb(80, 200, 200);
    if (k == "ENUMERATED") return rgb(240, 170, 70);
    if (k == "INTEGER" || k == "REAL" || k == "BOOLEAN" || k == "NULL") return rgb(240, 214, 90);
    if (k == "BIT STRING" || k == "OCTET STRING") return rgb(120, 200, 140);
    if (k == "OBJECT IDENTIFIER" || k == "RELATIVE-OID") return rgb(170, 130, 240);
    if (has(k, "String") || has(k, "Time")) return rgb(150, 220, 110);
    if (k == "reference") return rgb(170, 176, 190);
    if (k == "class" || k == "object" || k == "object field") return rgb(240, 120, 90);
    if (k == "value" || k == "value set") return rgb(92, 98, 116);
    return rgb(200, 200, 200);
}

const std::vector<std::pair<std::string, std::string>>& cityLegend() {
    static const std::vector<std::pair<std::string, std::string>> k = {
        {"SEQUENCE / SET", "SEQUENCE"}, {"CHOICE", "CHOICE"},       {"SEQUENCE OF / SET OF", "SEQUENCE OF"},
        {"ENUMERATED", "ENUMERATED"},   {"INTEGER, BOOLEAN", "INTEGER"}, {"BIT/OCTET STRING", "OCTET STRING"},
        {"OBJECT IDENTIFIER", "OBJECT IDENTIFIER"}, {"character strings, times", "IA5String"},
        {"Type2 ::= Type1", "reference"}, {"classes, objects", "class"}, {"values (flat)", "value"},
    };
    return k;
}

CityScene::CityScene(const Schema& schema, CityStyle style) : schema_(schema), style_(style) {
    lots_.resize(schema_.types.size());
    // Districts: a grid of lots per module, sorted by kind, then height.
    const float cell = style_.footprint + style_.gap;
    for (size_t m = 0; m < schema_.modules.size(); ++m) {
        District d;
        d.module = static_cast<int>(m);
        for (const SchemaType& t : schema_.types)
            if (t.module == static_cast<int>(m)) d.types.push_back(t.id);
        std::stable_sort(d.types.begin(), d.types.end(), [&](int a, int b) {
            const SchemaType& ta = schema_.types[static_cast<size_t>(a)];
            const SchemaType& tb = schema_.types[static_cast<size_t>(b)];
            if (kindRank(ta.kind) != kindRank(tb.kind)) return kindRank(ta.kind) < kindRank(tb.kind);
            return ta.members > tb.members;
        });
        const int n = std::max<int>(1, static_cast<int>(d.types.size()));
        const int cols = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<float>(n) * 1.3f))));
        const int rows = (n + cols - 1) / cols;
        d.x1 = static_cast<float>(cols) * cell - style_.gap + 2 * style_.pad;
        d.z1 = static_cast<float>(rows) * cell - style_.gap + 2 * style_.pad;
        districts_.push_back(std::move(d));
    }
    placeDistricts();

    meshes_.resize(districts_.size());
    for (size_t di = 0; di < districts_.size(); ++di) {
        District& d = districts_[di];
        MeshBuilder& mesh = meshes_[di];
        const Rgb ground = rgb(30, 34, 46);
        mesh.addTile(d.x0, d.z0, d.x1, d.z1, 0.0f, ground, -1);
        const int cols = std::max(1, static_cast<int>(std::lround((d.x1 - d.x0 - 2 * style_.pad + style_.gap) / cell)));
        for (size_t i = 0; i < d.types.size(); ++i) {
            const int t = d.types[i];
            const SchemaType& ty = schema_.types[static_cast<size_t>(t)];
            const int c = static_cast<int>(i) % cols, r = static_cast<int>(i) / cols;
            Lot& lot = lots_[static_cast<size_t>(t)];
            lot.district = static_cast<int>(di);
            lot.x0 = d.x0 + style_.pad + static_cast<float>(c) * cell;
            lot.z0 = d.z0 + style_.pad + static_cast<float>(r) * cell;
            lot.x1 = lot.x0 + style_.footprint;
            lot.z1 = lot.z0 + style_.footprint;
            lot.height = ty.flat() ? style_.flatHeight
                                   : style_.baseHeight + style_.heightScale * std::log2(1.0f + static_cast<float>(ty.members));
            tallest_ = std::max(tallest_, lot.height);
            const Rgb side = typeKindColor(ty.kind);
            const Rgb top{std::min(1.0f, side.r * 1.15f), std::min(1.0f, side.g * 1.15f), std::min(1.0f, side.b * 1.15f)};
            const float lo[3] = {lot.x0, 0.0f, lot.z0}, hi[3] = {lot.x1, lot.height, lot.z1};
            mesh.addBox(lo, hi, side, top, t);
            if (ty.scc >= 0) {
                // A glowing roof for recursion, legible under any light.
                const float rlo[3] = {lot.x0 - 0.06f, lot.height, lot.z0 - 0.06f};
                const float rhi[3] = {lot.x1 + 0.06f, lot.height + 0.22f, lot.z1 + 0.06f};
                roofs_.addBox(rlo, rhi, {1, 0.3f, 0.45f}, {1, 0.42f, 0.55f}, t);
            }
        }
    }
}

void CityScene::placeDistricts() {
    const size_t n = districts_.size();
    std::vector<float> cx(n), cz(n), hw(n), hd(n);
    float total = 0;
    for (size_t i = 0; i < n; ++i) {
        hw[i] = (districts_[i].x1 - districts_[i].x0) / 2;
        hd[i] = (districts_[i].z1 - districts_[i].z0) / 2;
        total += std::max(hw[i], hd[i]) * 2 + style_.districtGap;
    }
    // Start on a circle, in module order (deterministic: no randomness anywhere).
    const float radius = n > 1 ? total / 6.2832f : 0;
    for (size_t i = 0; i < n; ++i) {
        const float a = 6.2832f * static_cast<float>(i) / static_cast<float>(std::max<size_t>(1, n));
        cx[i] = radius * std::cos(a);
        cz[i] = radius * std::sin(a);
    }
    // IMPORTS pairs pull together.
    std::vector<std::pair<size_t, size_t>> links;
    for (size_t i = 0; i < n; ++i)
        for (const std::string& imp : schema_.modules[districts_[i].module].imports) {
            const int j = schema_.moduleByName(imp);
            if (j >= 0 && static_cast<size_t>(j) != i) links.emplace_back(i, static_cast<size_t>(j));
        }
    for (int it = 0; it < 400; ++it) {
        for (const auto& l : links) {
            const size_t a = l.first, b = l.second;
            const float dx = cx[b] - cx[a], dz = cz[b] - cz[a];
            const float dist = std::sqrt(dx * dx + dz * dz) + 1e-3f;
            const float want = std::max(hw[a], hd[a]) + std::max(hw[b], hd[b]) + style_.districtGap;
            const float k = 0.02f * (dist - want) / dist;
            cx[a] += dx * k;
            cz[a] += dz * k;
            cx[b] -= dx * k;
            cz[b] -= dz * k;
        }
        // Everything drifts gently to the middle, so unlinked districts stay near.
        for (size_t i = 0; i < n; ++i) {
            cx[i] *= 0.995f;
            cz[i] *= 0.995f;
        }
        // Overlaps push apart along the axis of least penetration.
        for (size_t a = 0; a < n; ++a)
            for (size_t b = a + 1; b < n; ++b) {
                const float ox = hw[a] + hw[b] + style_.districtGap - std::fabs(cx[b] - cx[a]);
                const float oz = hd[a] + hd[b] + style_.districtGap - std::fabs(cz[b] - cz[a]);
                if (ox <= 0 || oz <= 0) continue;
                if (ox < oz) {
                    const float s = (cx[b] >= cx[a] ? 1.0f : -1.0f) * ox / 2;
                    cx[a] -= s;
                    cx[b] += s;
                } else {
                    const float s = (cz[b] >= cz[a] ? 1.0f : -1.0f) * oz / 2;
                    cz[a] -= s;
                    cz[b] += s;
                }
            }
    }
    // Final pass: no overlap may remain.
    for (int it = 0; it < 200; ++it) {
        bool moved = false;
        for (size_t a = 0; a < n; ++a)
            for (size_t b = a + 1; b < n; ++b) {
                const float ox = hw[a] + hw[b] + style_.districtGap - std::fabs(cx[b] - cx[a]);
                const float oz = hd[a] + hd[b] + style_.districtGap - std::fabs(cz[b] - cz[a]);
                if (ox <= 0 || oz <= 0) continue;
                moved = true;
                if (ox < oz) {
                    const float s = (cx[b] >= cx[a] ? 1.0f : -1.0f) * (ox / 2 + 0.01f);
                    cx[a] -= s;
                    cx[b] += s;
                } else {
                    const float s = (cz[b] >= cz[a] ? 1.0f : -1.0f) * (oz / 2 + 0.01f);
                    cz[a] -= s;
                    cz[b] += s;
                }
            }
        if (!moved) break;
    }
    for (size_t i = 0; i < n; ++i) {
        District& d = districts_[i];
        d.x0 = std::round((cx[i] - hw[i]) * 100) / 100;
        d.z0 = std::round((cz[i] - hd[i]) * 100) / 100;
        d.x1 = d.x0 + 2 * hw[i];
        d.z1 = d.z0 + 2 * hd[i];
    }
}

Vec3 CityScene::roof(int type) const {
    const Lot& l = lots_[static_cast<size_t>(type)];
    return {(l.x0 + l.x1) / 2, l.height, (l.z0 + l.z1) / 2};
}

Vec3 CityScene::districtCentre(size_t d) const {
    const District& di = districts_[d];
    return {(di.x0 + di.x1) / 2, 0, (di.z0 + di.z1) / 2};
}

int CityScene::typeAt(size_t district, int32_t triangle) const {
    if (district >= meshes_.size()) return -1;
    return tagForTriangle(meshes_[district].tags(), triangle);
}

OrbitCamera CityScene::homeCamera(float aspect) const {
    float x0 = 1e9f, z0 = 1e9f, x1 = -1e9f, z1 = -1e9f;
    for (const District& d : districts_) {
        x0 = std::min(x0, d.x0);
        z0 = std::min(z0, d.z0);
        x1 = std::max(x1, d.x1);
        z1 = std::max(z1, d.z1);
    }
    OrbitCamera c;
    c.target = {(x0 + x1) / 2, 0, (z0 + z1) / 2};
    c.yawDeg = -28;
    c.pitchDeg = 42;
    const float w = x1 - x0, d = z1 - z0;
    const float half = std::max(w / 2 / std::max(aspect, 0.3f), d / 2) * 1.25f + tallest_;
    c.distance = half / std::tan(c.fovYDeg * 3.14159265f / 360) + 6;
    c.farPlane = c.distance * 4 + 500;
    return c;
}

std::vector<Vec3> arc(Vec3 a, Vec3 b, float lift, int n) {
    std::vector<Vec3> out;
    const Vec3 c{(a.x + b.x) / 2, std::max(a.y, b.y) + lift, (a.z + b.z) / 2};
    Vec3 prev = a;
    for (int i = 1; i <= n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n), u = 1 - t;
        const Vec3 p{u * u * a.x + 2 * u * t * c.x + t * t * b.x, u * u * a.y + 2 * u * t * c.y + t * t * b.y,
                     u * u * a.z + 2 * u * t * c.z + t * t * b.z};
        out.push_back(prev);
        out.push_back(p);
        prev = p;
    }
    return out;
}

}  // namespace asn1viz
