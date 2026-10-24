#include <cmath>

#include "check.h"
#include "scene/mesh.h"

using namespace asn1viz;

namespace {

// Every triangle's winding must agree with its stored normal.
bool windingMatchesNormals(const MeshBuilder& m) {
    const auto& p = m.positions();
    const auto& n = m.normals();
    const auto& idx = m.indices();
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const float* a = &p[idx[t] * 3];
        const float* b = &p[idx[t + 1] * 3];
        const float* c = &p[idx[t + 2] * 3];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float* nn = &n[idx[t] * 3];
        if (cr[0] * nn[0] + cr[1] * nn[1] + cr[2] * nn[2] <= 0) return false;
    }
    return true;
}

}  // namespace

int main() {
    MeshBuilder m;
    const float lo[3] = {0, 0, 0}, hi[3] = {1, 2, 3};
    m.addBox(lo, hi, {1, 0, 0}, {0, 1, 0}, 7);
    CHECK(m.vertexCount() == 24);
    CHECK(m.triangleCount() == 12);
    CHECK(windingMatchesNormals(m));
    const float lo2[3] = {2, 0, 0}, hi2[3] = {3, 1, 1};
    m.addBox(lo2, hi2, {1, 1, 1}, {1, 1, 1}, 9);
    m.addBox(lo2, hi2, {1, 1, 1}, {1, 1, 1}, 9);  // a second piece of the same thing
    m.addTile(0, 0, 4, 4, -0.1f, {0.2f, 0.2f, 0.2f}, 11);
    CHECK(windingMatchesNormals(m));
    CHECK(m.tags().size() == 3);  // the two pieces of 9 share one range
    CHECK(tagForTriangle(m.tags(), 0) == 7);
    CHECK(tagForTriangle(m.tags(), 11) == 7);
    CHECK(tagForTriangle(m.tags(), 12) == 9);
    CHECK(tagForTriangle(m.tags(), 35) == 9);
    CHECK(tagForTriangle(m.tags(), 36) == 11);
    CHECK(tagForTriangle(m.tags(), 37) == 11);
    CHECK(tagForTriangle(m.tags(), 38) == -1);
    CHECK(tagForTriangle(m.tags(), -1) == -1);
    // Indices stay in range.
    bool inRange = true;
    for (uint32_t i : m.indices()) inRange = inRange && static_cast<int32_t>(i) < m.vertexCount();
    CHECK(inRange);
    TEST_MAIN_END();
}
