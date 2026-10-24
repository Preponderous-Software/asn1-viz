// The schema city (RFC 0024 sections 2 and 4) as geometry, without an engine.
//
// Each module is a district on the ground plane, its area following its
// number of assignments, placed by a deterministic force layout over the
// IMPORTS graph. Each type assignment is a building on the district's grid,
// sorted by kind and then height, its height log-scaled from its member
// count; values, value sets and objects are flat tiles. One mesh per
// district (ground plate and buildings), tagged per type, so a ray hit's
// triangle names the type. Members of recursive groups get a glowing roof
// (a separate unlit mesh).
#pragma once

#include <string>
#include <vector>

#include "model/schema.h"
#include "scene/camera.h"
#include "scene/mesh.h"

namespace asn1viz {

struct CityStyle {
    float footprint = 1.0f;   // a building's side
    float gap = 0.8f;         // between buildings
    float pad = 2.2f;         // around a district's grid
    float districtGap = 8.0f; // at least this between districts
    float baseHeight = 0.5f;
    float heightScale = 1.5f; // per log2(1 + members)
    float flatHeight = 0.08f;
};

struct Lot {
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0, height = 0;
    int district = -1;
};

struct District {
    int module = 0;
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    std::vector<int> types;
};

// A family of kinds shares a colour.
Rgb typeKindColor(const std::string& kind);
const std::vector<std::pair<std::string, std::string>>& cityLegend();

class CityScene {
public:
    CityScene(const Schema& schema, CityStyle style);

    const Schema& schema() const { return schema_; }
    const std::vector<District>& districts() const { return districts_; }
    const Lot& lot(int type) const { return lots_[static_cast<size_t>(type)]; }
    const MeshBuilder& districtMesh(size_t d) const { return meshes_[d]; }
    const MeshBuilder& roofMesh() const { return roofs_; }

    Vec3 roof(int type) const;           // the centre of a building's roof
    Vec3 districtCentre(size_t d) const; // on the ground
    float tallest() const { return tallest_; }
    // Picks: a district mesh index and a triangle to a type id, or -1.
    int typeAt(size_t district, int32_t triangle) const;
    OrbitCamera homeCamera(float aspect) const;

private:
    void placeDistricts();

    Schema schema_;
    CityStyle style_;
    std::vector<District> districts_;
    std::vector<Lot> lots_;
    std::vector<MeshBuilder> meshes_;
    MeshBuilder roofs_;
    float tallest_ = 0;
};

// A quadratic arc from a to b, lifted by `lift` at the middle, as n segments
// (2n line vertices' positions, pairwise).
std::vector<Vec3> arc(Vec3 a, Vec3 b, float lift, int n);

}  // namespace asn1viz
