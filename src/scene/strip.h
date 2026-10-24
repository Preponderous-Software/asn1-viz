// The bit strips (RFC 0023 section 4) as geometry, without an engine.
//
// Each lane (UPER, APER, an imported encoding) is one mesh: one box per bit,
// wrapped into rows of `rowBits` bits like a hex dump, the rows running
// along +X and stacking along +Z. Lanes sit side by side along +X. A box's
// colour is its segment's kind (values by ASN.1 type family), its height is
// the nesting depth of its frame, ones stand a little taller than zeros, and
// inferred segments have a darker top. Every box is tagged with its segment
// index, so a ray hit's triangle names the segment.
#pragma once

#include <string>
#include <vector>

#include "model/layout.h"
#include "scene/camera.h"
#include "scene/mesh.h"

namespace asn1viz {

struct StripStyle {
    int rowBits = 256;      // bits per row
    float bitPitch = 1.0f;  // along X
    float rowPitch = 1.6f;  // along Z
    float laneGap = 24.0f;  // between lanes, along X
    float baseHeight = 0.35f;
    float depthStep = 0.32f;
    float oneBonus = 0.22f; // a 1 bit stands this much taller
};

// The colour of a segment kind (and, for values, of the frame's type).
Rgb kindColor(const std::string& kind, const std::string& type);
// The legend's entries, in display order: (label, kind).
const std::vector<std::pair<std::string, std::string>>& legendKinds();

// Row-major placement of one bit of one lane.
struct BitPlace {
    float x0, x1, z0, z1;
};

class StripScene {
public:
    StripScene(const Layout& layout, StripStyle style);

    const Layout& layout() const { return layout_; }
    const StripStyle& style() const { return style_; }
    size_t laneCount() const { return layout_.lanes.size(); }
    const MeshBuilder& laneMesh(size_t lane) const { return meshes_[lane]; }

    BitPlace place(size_t lane, int64_t bit) const;
    float laneX0(size_t lane) const;
    float laneWidth() const { return static_cast<float>(style_.rowBits) * style_.bitPitch; }
    float laneDepth(size_t lane) const;
    // Top of the boxes of a segment (its frame's depth), without the 1 bonus.
    float segmentHeight(size_t lane, int segment) const;
    float frameHeight(size_t lane, int frame) const;

    // The centre of a bit range's first box, at its top (for labels and links).
    Vec3 anchor(size_t lane, int64_t bit, float height) const;
    // Outline boxes for a bit range: one box per row it touches (x0,z0,x1,z1).
    std::vector<BitPlace> rowSpans(size_t lane, int64_t start, int64_t len) const;

    // The frame in another lane with the same path, or -1.
    int matchingFrame(size_t fromLane, int frame, size_t toLane) const;

    // A camera that frames every lane.
    OrbitCamera homeCamera(float aspect) const;

private:
    Layout layout_;
    StripStyle style_;
    std::vector<MeshBuilder> meshes_;
};

}  // namespace asn1viz
