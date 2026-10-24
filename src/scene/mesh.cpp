#include "scene/mesh.h"

#include <algorithm>

namespace asn1viz {

void MeshBuilder::quad(const float a[3], const float b[3], const float c[3], const float d[3], const float n[3],
                       Rgb color) {
    const uint32_t base = static_cast<uint32_t>(positions_.size() / 3);
    for (const float* p : {a, b, c, d}) {
        positions_.insert(positions_.end(), {p[0], p[1], p[2]});
        normals_.insert(normals_.end(), {n[0], n[1], n[2]});
        colors_.insert(colors_.end(), {color.r, color.g, color.b});
    }
    // a, b, c, d run counter-clockwise seen from the side n points to.
    indices_.insert(indices_.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void MeshBuilder::tagLast(int32_t triangles, int32_t tag) {
    const int32_t first = triangleCount() - triangles;
    if (!tags_.empty() && tags_.back().tag == tag &&
        tags_.back().firstTriangle + tags_.back().triangleCount == first) {
        tags_.back().triangleCount += triangles;  // a box split in pieces keeps one range
        return;
    }
    tags_.push_back({first, triangles, tag});
}

void MeshBuilder::addBox(const float lo[3], const float hi[3], Rgb side, Rgb top, int32_t tag) {
    const float x0 = lo[0], y0 = lo[1], z0 = lo[2], x1 = hi[0], y1 = hi[1], z1 = hi[2];
    const float p000[3] = {x0, y0, z0}, p100[3] = {x1, y0, z0}, p110[3] = {x1, y1, z0}, p010[3] = {x0, y1, z0};
    const float p001[3] = {x0, y0, z1}, p101[3] = {x1, y0, z1}, p111[3] = {x1, y1, z1}, p011[3] = {x0, y1, z1};
    const float nx[3] = {1, 0, 0}, mx[3] = {-1, 0, 0}, ny[3] = {0, 1, 0}, my[3] = {0, -1, 0}, nz[3] = {0, 0, 1},
                mz[3] = {0, 0, -1};
    quad(p001, p101, p111, p011, nz, side);  // +Z
    quad(p100, p000, p010, p110, mz, side);  // -Z
    quad(p101, p100, p110, p111, nx, side);  // +X
    quad(p000, p001, p011, p010, mx, side);  // -X
    quad(p011, p111, p110, p010, ny, top);   // +Y
    quad(p000, p100, p101, p001, my, side);  // -Y
    tagLast(12, tag);
}

void MeshBuilder::addTile(float x0, float z0, float x1, float z1, float y, Rgb color, int32_t tag) {
    const float a[3] = {x0, y, z1}, b[3] = {x1, y, z1}, c[3] = {x1, y, z0}, d[3] = {x0, y, z0};
    const float n[3] = {0, 1, 0};
    quad(a, b, c, d, n, color);
    tagLast(2, tag);
}

int32_t tagForTriangle(const std::vector<TriTag>& tags, int32_t triangle) {
    if (triangle < 0) return -1;
    auto it = std::upper_bound(tags.begin(), tags.end(), triangle,
                               [](int32_t t, const TriTag& r) { return t < r.firstTriangle; });
    if (it == tags.begin()) return -1;
    --it;
    return triangle < it->firstTriangle + it->triangleCount ? it->tag : -1;
}

}  // namespace asn1viz
