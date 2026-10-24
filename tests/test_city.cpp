// The city's layout: districts never overlap, every building stands on its
// own district, each building's triangles name its type, recursive types get
// a roof, and the same schema always gives the same city.
#include <cmath>
#include <string>

#include "check.h"
#include "model/schema.h"
#include "scene/city.h"

using namespace asn1viz;

namespace {

void checkCity(const Schema& s) {
    const CityScene c(s, CityStyle{});
    const auto& ds = c.districts();
    CHECK(ds.size() == s.modules.size());
    for (size_t a = 0; a < ds.size(); ++a)
        for (size_t b = a + 1; b < ds.size(); ++b) {
            const bool apart = ds[a].x1 <= ds[b].x0 || ds[b].x1 <= ds[a].x0 || ds[a].z1 <= ds[b].z0 || ds[b].z1 <= ds[a].z0;
            CHECK(apart);
        }
    bool onDistrict = true, tagged = true;
    for (const SchemaType& t : s.types) {
        const Lot& l = c.lot(t.id);
        const District& d = ds[static_cast<size_t>(l.district)];
        onDistrict = onDistrict && l.x0 >= d.x0 && l.x1 <= d.x1 && l.z0 >= d.z0 && l.z1 <= d.z1 && d.module == t.module;
    }
    CHECK(onDistrict);
    // Triangle tags: the ground plate is -1, then each building's 12 triangles.
    for (size_t d = 0; d < ds.size(); ++d) {
        const MeshBuilder& m = c.districtMesh(d);
        CHECK(c.typeAt(d, 0) == -1);
        for (size_t i = 0; i < ds[d].types.size(); ++i)
            tagged = tagged && c.typeAt(d, static_cast<int32_t>(2 + i * 12 + 7)) == ds[d].types[i];
        CHECK(m.triangleCount() == static_cast<int32_t>(2 + ds[d].types.size() * 12));
    }
    CHECK(tagged);
    size_t recursive = 0;
    for (const SchemaType& t : s.types) recursive += t.scc >= 0 ? 1 : 0;
    CHECK(c.roofMesh().triangleCount() == static_cast<int32_t>(recursive * 12));
    // Deterministic.
    const CityScene again(s, CityStyle{});
    bool same = true;
    for (size_t d = 0; d < ds.size(); ++d)
        same = same && ds[d].x0 == again.districts()[d].x0 && ds[d].z0 == again.districts()[d].z0;
    CHECK(same);
    // The home camera sees every district.
    const CameraMatrices m = matrices(c.homeCamera(16.0f / 9.0f), 16.0f / 9.0f);
    for (size_t d = 0; d < ds.size(); ++d) {
        float x, y;
        CHECK(toScreen(m, c.districtCentre(d), 1280, 720, x, y));
    }
    // Arcs are joined segments from a to b.
    const auto pts = arc({0, 0, 0}, {10, 0, 0}, 4, 8);
    CHECK(pts.size() == 16 && pts.front().x == 0 && std::fabs(pts.back().x - 10) < 1e-4f && pts[8].y > 1.9f);  // a quadratic arc peaks at half its lift
}

}  // namespace

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    checkCity(loadSchemaFile(root + "/datasets/pkix/schema.json"));
    checkCity(loadSchemaFile(root + "/datasets/recursion/schema.json"));
    TEST_MAIN_END();
}
