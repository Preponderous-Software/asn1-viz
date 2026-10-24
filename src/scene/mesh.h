// Triangle meshes built from boxes, with a tag per box so a ray hit's
// triangle index turns back into whatever the box stands for (a segment of a
// bit strip, a building of the schema city). No engine: the view uploads
// these to Strata as StrataMesh arrays.
//
// The tag table is the pattern Kindling uses for its targets: a list of
// (first triangle, triangle count, tag) ranges in triangle order.
#pragma once

#include <cstdint>
#include <vector>

namespace asn1viz {

struct Rgb {
    float r = 1, g = 1, b = 1;
};

struct TriTag {
    int32_t firstTriangle = 0;
    int32_t triangleCount = 0;
    int32_t tag = -1;
};

class MeshBuilder {
public:
    // An axis-aligned box from lo to hi, flat shaded (24 vertices, 12
    // triangles), wound counter-clockwise seen from outside. `top` colours
    // the +Y face, `side` the other five.
    void addBox(const float lo[3], const float hi[3], Rgb side, Rgb top, int32_t tag);

    // A flat quad facing +Y at height y (4 vertices, 2 triangles).
    void addTile(float x0, float z0, float x1, float z1, float y, Rgb color, int32_t tag);

    const std::vector<float>& positions() const { return positions_; }
    const std::vector<float>& normals() const { return normals_; }
    const std::vector<float>& colors() const { return colors_; }
    const std::vector<uint32_t>& indices() const { return indices_; }
    const std::vector<TriTag>& tags() const { return tags_; }
    int32_t vertexCount() const { return static_cast<int32_t>(positions_.size() / 3); }
    int32_t triangleCount() const { return static_cast<int32_t>(indices_.size() / 3); }
    bool empty() const { return indices_.empty(); }

private:
    void quad(const float a[3], const float b[3], const float c[3], const float d[3], const float n[3], Rgb color);
    void tagLast(int32_t triangles, int32_t tag);

    std::vector<float> positions_, normals_, colors_;
    std::vector<uint32_t> indices_;
    std::vector<TriTag> tags_;
};

// The tag of the box a triangle belongs to, or -1. `tags` must be in
// triangle order, as MeshBuilder writes them.
int32_t tagForTriangle(const std::vector<TriTag>& tags, int32_t triangle);

}  // namespace asn1viz
